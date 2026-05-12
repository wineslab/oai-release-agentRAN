-- DL Proportional Fair Scheduler with Pipe-based Metrics Logging
-- OLLA MCS adaptation + token bucket rate limiting + batched JSON metrics
--
-- Writes to /tmp/dl_metrics.pipe in compact format for external processing
-- Start the reader BEFORE the scheduler: mkfifo /tmp/dl_metrics.pipe

local ffi = require("ffi")
local bit = require("bit")
local socket = require("socket")

ffi.cdef[[
typedef struct {
    uint16_t rnti;
    uint8_t nr_of_layers;
    uint32_t pending_bytes;
    float throughput;
    uint8_t previous_mcs;
    int ue_type;
    uint16_t required_rbs;
    uint8_t required_mcs;
    uint16_t cqi;
    int uid;
    int mcs_table;
    float bler;
    int slot;
    int frame;
    uint64_t fiveQI;
    int16_t channel_mag_per_rb[272];
    uint32_t bwp_start;
    uint32_t bwp_size;
    int16_t dl_rsrp;
    uint64_t hol_delay_us;
    uint16_t allocated_rb;
    uint8_t allocated_mcs;
    uint16_t allocated_rb_start;
} dl_ue_metric_t;

// POSIX file operations for non-blocking I/O
int open(const char *pathname, int flags, ...);
int close(int fd);
ssize_t write(int fd, const void *buf, size_t count);
int fcntl(int fd, int cmd, ...);
char *strerror(int errnum);
]]

-- POSIX constants for non-blocking I/O
local O_WRONLY = 1
local O_NONBLOCK = 2048
local EAGAIN = 11

if ffi.os == "OSX" then
    O_NONBLOCK = 4
    EAGAIN = 35
end

local UE_TYPE_NEW_DATA = 0
local UE_TYPE_HARQ_RETX = 1

---------------------------------------------------------------------------
-- OLLA state per RNTI
---------------------------------------------------------------------------
local olla = {}

local MAX_FRAME    = 1024
local OLLA_PERIOD  = 10     -- adjust every 10 frames (100ms)
local BLER_TARGET  = 0.10
local OLLA_UP      = 0.10
local OLLA_DOWN    = 1.00
local MIN_MCS      = 0

local function frames_elapsed(now, last)
    return (now - last) % MAX_FRAME
end

local function olla_mcs(rnti, seed_mcs, bler, mcs_table, frame)
    local max_mcs = 27
    local s = olla[rnti]

    if not s then
        local init = seed_mcs > 0 and seed_mcs or 4
        s = { frac = init + 0.0, last_frame = frame }
        olla[rnti] = s
    end

    if frames_elapsed(frame, s.last_frame) >= OLLA_PERIOD then
        if bler < BLER_TARGET then
            s.frac = s.frac + OLLA_UP
        else
            s.frac = s.frac - OLLA_DOWN
        end
        s.last_frame = frame
    end

    s.frac = math.max(MIN_MCS, math.min(s.frac, max_mcs))
    return math.floor(s.frac)
end

---------------------------------------------------------------------------
-- RB mask helpers
---------------------------------------------------------------------------

local function find_free_rbs(mask, needed, bwp_start, bwp_size)
    local count = 0
    for rb = 0, bwp_size - 1 do
        local idx = bwp_start + rb + 1
        if mask:sub(idx, idx) ~= 'X' then
            count = count + 1
            if count == needed then
                return rb - needed + 1
            end
        else
            count = 0
        end
    end
    return -1
end

local function mark_used(mask, start_rb, num_rbs, bwp_start)
    local chars = {}
    for i = 1, #mask do chars[i] = mask:sub(i, i) end
    for rb = start_rb, start_rb + num_rbs - 1 do
        chars[bwp_start + rb + 1] = 'X'
    end
    return table.concat(chars)
end

local function find_largest_block(mask, bwp_start, bwp_size)
    local best_start, best_len = -1, 0
    local cur_start, cur_len = -1, 0
    for rb = 0, bwp_size - 1 do
        local idx = bwp_start + rb + 1
        if mask:sub(idx, idx) ~= 'X' then
            if cur_len == 0 then cur_start = rb end
            cur_len = cur_len + 1
        else
            if cur_len > best_len then
                best_start = cur_start
                best_len = cur_len
            end
            cur_len = 0
        end
    end
    if cur_len > best_len then
        best_start = cur_start
        best_len = cur_len
    end
    return best_start, best_len
end

---------------------------------------------------------------------------
-- Pipe I/O (non-blocking, with overflow buffer)
---------------------------------------------------------------------------
local PIPE_PATH = "/tmp/dl_metrics.pipe"
local pipe_fd = -1
local pipe_open_attempted = false
local pipe_retry_counter = 0
local PIPE_RETRY_INTERVAL = 1000

local overflow_buffer = {}
local MAX_OVERFLOW_SIZE = 1000
local overflow_drops = 0

local string_buffer = {}

local function get_timestamp_ns()
    return math.floor(socket.gettime() * 1e9)
end

local function get_errno()
    return ffi.errno()
end

local function pipe_write_nonblock(data)
    if pipe_fd < 0 then return nil end
    local len = #data
    local ret = ffi.C.write(pipe_fd, data, len)
    if ret < 0 then
        local err = get_errno()
        if err == EAGAIN then
            return -1
        else
            print("[DL Scheduler] Pipe write error: " .. ffi.string(ffi.C.strerror(err)))
            ffi.C.close(pipe_fd)
            pipe_fd = -1
            pipe_open_attempted = false
            return nil
        end
    end
    return ret
end

local function drain_overflow_buffer()
    while #overflow_buffer > 0 do
        local data = overflow_buffer[1]
        local ret = pipe_write_nonblock(data)
        if ret == nil then
            overflow_buffer = {}
            return
        elseif ret == -1 then
            return
        elseif ret < #data then
            overflow_buffer[1] = data:sub(ret + 1)
            return
        else
            table.remove(overflow_buffer, 1)
        end
    end
end

local function pipe_write_buffered(data)
    if pipe_fd < 0 then return end
    drain_overflow_buffer()
    if #overflow_buffer == 0 then
        local ret = pipe_write_nonblock(data)
        if ret == nil then
            return
        elseif ret == -1 then
            table.insert(overflow_buffer, data)
        elseif ret < #data then
            table.insert(overflow_buffer, data:sub(ret + 1))
        end
    else
        table.insert(overflow_buffer, data)
    end
    while #overflow_buffer > MAX_OVERFLOW_SIZE do
        table.remove(overflow_buffer, 1)
        overflow_drops = overflow_drops + 1
        if overflow_drops % 1000 == 0 then
            print("[DL Scheduler] Warning: dropped " .. overflow_drops .. " messages (pipe reader too slow)")
        end
    end
end

local function ensure_pipe_open()
    if pipe_fd >= 0 then return true end
    if pipe_open_attempted then return false end
    local fd = ffi.C.open(PIPE_PATH, bit.bor(O_WRONLY, O_NONBLOCK))
    if fd >= 0 then
        pipe_fd = fd
        overflow_buffer = {}
        overflow_drops = 0
        print("[DL Scheduler] Connected to metrics pipe (non-blocking): " .. PIPE_PATH)
        return true
    else
        if not pipe_open_attempted then
            print("[DL Scheduler] Pipe not available: " .. PIPE_PATH .. " (create with: mkfifo " .. PIPE_PATH .. ")")
            pipe_open_attempted = true
        end
        return false
    end
end

local function maybe_retry_pipe()
    if pipe_fd >= 0 then return end
    pipe_retry_counter = pipe_retry_counter + 1
    if pipe_retry_counter >= PIPE_RETRY_INTERVAL then
        pipe_retry_counter = 0
        pipe_open_attempted = false
        ensure_pipe_open()
    end
end

---------------------------------------------------------------------------
-- Metrics logging (batched JSON per slot)
---------------------------------------------------------------------------
local LOG_EVERY_N_SLOTS = 200
local slot_counter = 0

local function log_metrics_batched(metrics, n_ues, frame, slot, ts, rb_mask)
    if not ensure_pipe_open() then return end

    for i = 1, #string_buffer do string_buffer[i] = nil end
    local idx = 1

    if rb_mask then
        string_buffer[idx] = string.format('{"ts":%d,"f":%d,"s":%d,"mask":"%s","ues":[', ts, frame, slot, rb_mask)
    else
        string_buffer[idx] = string.format('{"ts":%d,"f":%d,"s":%d,"ues":[', ts, frame, slot)
    end
    idx = idx + 1

    for i = 0, n_ues - 1 do
        local m = metrics[i]
        if i > 0 then
            string_buffer[idx] = ","
            idx = idx + 1
        end

        string_buffer[idx] = string.format(
            '{"r":%d,"uid":%d,"t":%d,"qi":%d,"pb":%d,"tp":%.2f,"bl":%.4f,"cqi":%d,"rsrp":%d,"mcs_p":%d,"rbs_req":%d,"mcs_req":%d,"rbs_out":%d,"mcs_out":%d,"rb_st":%d,"bwp_st":%d,"bwp_sz":%d,"hol":%d}',
            m.rnti,
            m.uid,
            m.ue_type,
            tonumber(m.fiveQI),
            m.pending_bytes,
            m.throughput,
            m.bler,
            m.cqi,
            m.dl_rsrp,
            m.previous_mcs,
            m.required_rbs,
            m.required_mcs,
            m.allocated_rb,
            m.allocated_mcs,
            m.allocated_rb_start,
            m.bwp_start,
            m.bwp_size,
            tonumber(m.hol_delay_us)
        )
        idx = idx + 1
    end

    string_buffer[idx] = "]}\n"

    local line = table.concat(string_buffer)
    pipe_write_buffered(line)
end

---------------------------------------------------------------------------
-- Token bucket rate limiting
---------------------------------------------------------------------------

-- NR PDSCH MCS Table 1 spectral efficiency (bits/RE) from 3GPP TS 38.214
local mcs_se = {
    [0]=0.23, 0.31, 0.38, 0.49, 0.60, 0.74, 0.88, 1.03, 1.18, 1.33,
    1.33, 1.48, 1.70, 1.91, 2.16, 2.41, 2.57, 2.57, 2.73, 3.03,
    3.32, 3.61, 3.90, 4.21, 4.52, 4.82, 5.12, 5.33
}
local DATA_RE_PER_RB = 12 * 12  -- 12 subcarriers x 12 data symbols

local ue_bucket = {}
local BURST_WINDOW = 0.05
local INITIAL_SCALE = 0.7
local CAL_WINDOW = 0.5
local CAL_ALPHA = 0.3

-- Classification: fiveQI == 9 -> FWA (broadband), otherwise -> MTC
local function get_throughput_limit_bps(fiveQI)
    local limit_mbps
    if fiveQI == 9 then
        limit_mbps = fwa_max_throughput  -- global set by C via set_lua_dl_scheduler_config()
    else
        limit_mbps = mtc_max_throughput
    end
    if limit_mbps and limit_mbps > 0 then
        return limit_mbps * 1e6 / 8
    end
    return nil
end

---------------------------------------------------------------------------
-- Main entry point called from C every DL slot
---------------------------------------------------------------------------
function compute_dl_allocations(metrics_ptr, n_ues, total_rbs, min_rbs, rb_mask_string)
    local metrics = ffi.cast("dl_ue_metric_t*", metrics_ptr)
    local mask = rb_mask_string
    local now = socket.gettime()

    local frame = 0
    local slot = 0
    if n_ues > 0 then
        frame = metrics[0].frame
        slot = metrics[0].slot
    end

    -- Phase 1: HARQ retransmissions (exact RBs, highest priority, original MCS)
    for i = 0, n_ues - 1 do
        local m = metrics[i]
        if m.ue_type == UE_TYPE_HARQ_RETX and m.required_rbs > 0 then
            local s = find_free_rbs(mask, m.required_rbs, m.bwp_start, m.bwp_size)
            if s >= 0 then
                m.allocated_rb = m.required_rbs
                m.allocated_mcs = m.required_mcs
                m.allocated_rb_start = s
                mask = mark_used(mask, s, m.required_rbs, m.bwp_start)
            end
        end
    end

    -- Phase 2: New data UEs sorted by PF coefficient
    local pf = {}
    for i = 0, n_ues - 1 do
        local m = metrics[i]
        if m.ue_type == UE_TYPE_NEW_DATA then
            local thr = m.throughput > 0 and m.throughput or 1.0
            local coef = (m.previous_mcs + 1) / thr
            pf[#pf + 1] = { idx = i, coef = coef }
        end
    end
    table.sort(pf, function(a, b) return a.coef > b.coef end)

    for _, entry in ipairs(pf) do
        local m = metrics[entry.idx]
        local best_start, best_len = find_largest_block(mask, m.bwp_start, m.bwp_size)

        if best_len >= min_rbs then
            local mcs = olla_mcs(m.rnti, m.previous_mcs, m.bler, m.mcs_table, m.frame)
            local alloc_rbs = best_len
            local rnti = tonumber(m.rnti)

            -- Token bucket rate limiting
            local skip_ue = false
            local limit = get_throughput_limit_bps(tonumber(m.fiveQI))
            if limit then
                local b = ue_bucket[rnti]
                if not b then
                    b = {budget = limit * BURST_WINDOW, last_time = now,
                         scale = INITIAL_SCALE, est_sum = 0, tp_sum = 0, cal_time = now}
                    ue_bucket[rnti] = b
                end

                local dt = now - b.last_time
                if dt > 0 then
                    b.budget = b.budget + limit * dt
                    b.budget = math.min(b.budget, limit * BURST_WINDOW)
                    b.last_time = now
                end

                if dt > 0 and m.throughput > 0 then
                    b.tp_sum = b.tp_sum + (m.throughput / 8) * dt
                    if (now - b.cal_time) >= CAL_WINDOW and b.est_sum > 0 then
                        local ratio = b.tp_sum / b.est_sum
                        b.scale = b.scale * (1 - CAL_ALPHA) + (b.scale * ratio) * CAL_ALPHA
                        b.scale = math.max(0.1, math.min(2.0, b.scale))
                        b.est_sum = 0
                        b.tp_sum = 0
                        b.cal_time = now
                    end
                end

                local se = mcs_se[mcs] or 2.0
                local bytes_per_rb = m.nr_of_layers * se * DATA_RE_PER_RB / 8 * b.scale

                if bytes_per_rb > 0 then
                    local max_rbs = math.floor(b.budget / bytes_per_rb)
                    if max_rbs < min_rbs then
                        skip_ue = true
                    else
                        alloc_rbs = math.min(alloc_rbs, max_rbs)
                    end
                end

                if not skip_ue then
                    local deducted = alloc_rbs * bytes_per_rb
                    b.budget = b.budget - deducted
                    b.est_sum = b.est_sum + deducted
                end
            end

            if not skip_ue then
                m.allocated_rb = alloc_rbs
                m.allocated_mcs = mcs
                m.allocated_rb_start = best_start
                mask = mark_used(mask, best_start, alloc_rbs, m.bwp_start)
            end
        end
    end

    -- Log metrics (after allocation decisions are made)
    slot_counter = slot_counter + 1
    if slot_counter >= LOG_EVERY_N_SLOTS then
        slot_counter = 0
        local ts = get_timestamp_ns()
        log_metrics_batched(metrics, n_ues, frame, slot, ts, rb_mask_string)
    end

    maybe_retry_pipe()
end
