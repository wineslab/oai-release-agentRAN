-- UL Proportional Fair Scheduler with Pinned Allocations
-- For dataset collection: runs PF every PIN_INTERVAL_SLOTS, replays the same
-- allocation in between. All UE types (HARQ, no-data, new-data) respect
-- pinned PRB positions. Leftover resources are NOT redistributed.
--
-- Writes to /tmp/ul_metrics.pipe in compact format for external processing
-- Start the reader BEFORE the scheduler: mkfifo /tmp/ul_metrics.pipe
--
-- Switch from normal scheduler by setting:
--   export LUA_SCHED_UL=/path/to/pf_ul_pinned_logger.lua

local ffi = require("ffi")
local bit = require("bit")        -- LuaJIT bit operations
local socket = require("socket")  -- For high-precision timestamps

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

    int target_snrx10;
    uint16_t cqi;
    uint16_t rssi;

    int uid;
    int mcs_table;

    float bler;
    int pusch_snrx10;

    int slot;
    int frame;
    uint64_t fiveQI;
    int16_t channel_mag_per_rb[272];

    uint32_t bwp_start;
    uint32_t bwp_size;

    int16_t dl_rsrp;

    uint64_t unused_reserved;

    uint64_t rlc_arrival_bytes;
    uint32_t rlc_arrival_pkts;
    uint32_t rlc_dropped_pkts;
    uint32_t rlc_dropped_bytes;

    uint16_t allocated_rb;
    uint8_t allocated_mcs;
    uint16_t allocated_rb_start;
    char ul_prb_mask[276];
} ue_metric_ul_t;

// POSIX file operations for non-blocking I/O
int open(const char *pathname, int flags, ...);
int close(int fd);
ssize_t write(int fd, const void *buf, size_t count);
int fcntl(int fd, int cmd, ...);
char *strerror(int errnum);
]]

-- POSIX constants for non-blocking I/O (Linux/macOS)
local O_WRONLY = 1
local O_NONBLOCK = 2048      -- Linux: 04000, but 2048 works on both
local F_GETFL = 3
local F_SETFL = 4
local EAGAIN = 11            -- Resource temporarily unavailable

-- Platform detection for O_NONBLOCK (differs between Linux and macOS)
if ffi.os == "OSX" then
    O_NONBLOCK = 4           -- macOS uses 0x0004 for O_NONBLOCK
    EAGAIN = 35              -- macOS EAGAIN
end

-- UE type constants
local UE_TYPE_NEW_DATA = 0
local UE_TYPE_HARQ_RETX = 1
local UE_TYPE_NO_DATA = 2

-- BLER thresholds
local BLER_TARGET_LOW = 0.05
local BLER_TARGET_HIGH = 0.15

-- ============ PINNED ALLOCATION CONFIG ============
local PIN_INTERVAL_SLOTS = 200   -- 100ms at SCS 30kHz (0.5ms/slot)
local pin_counter = 0
local pin_window_id = 0
local pinned_allocs = {}         -- rnti -> {rb_start, num_rbs, mcs}

-- Pipe configuration
local PIPE_PATH = "/tmp/ul_metrics.pipe"
local pipe_fd = -1                    -- File descriptor for non-blocking writes
local pipe_open_attempted = false
local pipe_retry_counter = 0
local PIPE_RETRY_INTERVAL = 1000  -- slots between retry attempts

-- Overflow buffer for when pipe is full (non-blocking writes)
local overflow_buffer = {}            -- Queue of strings waiting to be written
local MAX_OVERFLOW_SIZE = 1000        -- Max queued messages before dropping oldest
local overflow_drops = 0              -- Counter for dropped messages

-- Logging configuration
local LOG_EVERY_N_SLOTS = 1           -- Log scheduler metrics every Nth slot
local LOG_CHANNEL_MAG = false         -- Disabled: channel mag now collected separately from C code
local slot_counter = 0

-- Pre-allocated string buffer to avoid GC pressure
local string_buffer = {}

-- Get timestamp in nanoseconds
local function get_timestamp_ns()
    return math.floor(socket.gettime() * 1e9)
end

-- Get errno (platform-specific)
local function get_errno()
    return ffi.errno()
end

-- Non-blocking write to pipe, returns bytes written or -1 on EAGAIN, nil on error
local function pipe_write_nonblock(data)
    if pipe_fd < 0 then return nil end

    local len = #data
    local ret = ffi.C.write(pipe_fd, data, len)

    if ret < 0 then
        local err = get_errno()
        if err == EAGAIN then
            return -1  -- Would block, need to buffer
        else
            -- Real error, close pipe
            print("[UL Scheduler Pinned] Pipe write error: " .. ffi.string(ffi.C.strerror(err)))
            ffi.C.close(pipe_fd)
            pipe_fd = -1
            pipe_open_attempted = false
            return nil
        end
    end

    return ret
end

-- Drain overflow buffer (try to write pending data)
local function drain_overflow_buffer()
    while #overflow_buffer > 0 do
        local data = overflow_buffer[1]
        local ret = pipe_write_nonblock(data)

        if ret == nil then
            -- Pipe error, clear buffer
            overflow_buffer = {}
            return
        elseif ret == -1 then
            -- Would block, stop draining
            return
        elseif ret < #data then
            -- Partial write, update first entry
            overflow_buffer[1] = data:sub(ret + 1)
            return
        else
            -- Full write, remove from queue
            table.remove(overflow_buffer, 1)
        end
    end
end

-- Write data to pipe with overflow buffering (never blocks)
local function pipe_write_buffered(data)
    if pipe_fd < 0 then return end

    -- First try to drain any pending data
    drain_overflow_buffer()

    -- If buffer is empty, try direct write
    if #overflow_buffer == 0 then
        local ret = pipe_write_nonblock(data)

        if ret == nil then
            return  -- Pipe error
        elseif ret == -1 then
            -- Would block, buffer the data
            table.insert(overflow_buffer, data)
        elseif ret < #data then
            -- Partial write, buffer remainder
            table.insert(overflow_buffer, data:sub(ret + 1))
        end
        -- Full write succeeded, nothing to do
    else
        -- Buffer not empty, queue this data
        table.insert(overflow_buffer, data)
    end

    -- Enforce max buffer size (drop oldest if needed)
    while #overflow_buffer > MAX_OVERFLOW_SIZE do
        table.remove(overflow_buffer, 1)
        overflow_drops = overflow_drops + 1
        if overflow_drops % 1000 == 0 then
            print("[UL Scheduler Pinned] Warning: dropped " .. overflow_drops .. " messages (pipe reader too slow)")
        end
    end
end

-- Try to open pipe with non-blocking I/O
local function ensure_pipe_open()
    if pipe_fd >= 0 then return true end
    if pipe_open_attempted then return false end

    -- Open pipe with O_WRONLY | O_NONBLOCK
    local fd = ffi.C.open(PIPE_PATH, bit.bor(O_WRONLY, O_NONBLOCK))
    if fd >= 0 then
        pipe_fd = fd
        overflow_buffer = {}
        overflow_drops = 0
        print("[UL Scheduler Pinned] Connected to metrics pipe (non-blocking): " .. PIPE_PATH)
        return true
    else
        if not pipe_open_attempted then
            print("[UL Scheduler Pinned] Pipe not available: " .. PIPE_PATH .. " (create with: mkfifo " .. PIPE_PATH .. ")")
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

-- Pre-allocated buffer for channel magnitude formatting (avoids GC in critical path)
local channel_mag_parts = {}
for i = 1, 272 do channel_mag_parts[i] = "" end

-- Format channel magnitude as comma-separated values
-- Uses pre-allocated buffer to avoid GC pressure
local function format_channel_mag(metric)
    local bwp_size = metric.bwp_size
    if bwp_size == 0 then bwp_size = 51 end  -- Default assumption

    for rb = 0, bwp_size - 1 do
        channel_mag_parts[rb + 1] = metric.channel_mag_per_rb[rb]
    end
    -- table.concat with numbers is faster than tostring() per element
    return table.concat(channel_mag_parts, ",", 1, bwp_size)
end

-- Log all metrics for all UEs in a single batched write
-- Format: One JSON-like line per slot containing all UEs and their metrics
-- include_channel_mag: whether to include per-RB channel magnitude this time
-- rb_mask: the RB mask string showing blocked/free PRBs for this slot
-- is_recompute: whether this slot was a PF recompute (true) or replay (false)
local function log_metrics_batched(metrics, n_ues, frame, slot, ts, include_channel_mag, rb_mask, is_recompute)
    if not ensure_pipe_open() then return end

    -- Clear buffer
    for i = 1, #string_buffer do string_buffer[i] = nil end
    local idx = 1

    -- Header: timestamp, frame, slot, pin info, PRB mask
    local pin_flag = is_recompute and 1 or 0
    if rb_mask then
        string_buffer[idx] = string.format('{"ts":%d,"f":%d,"s":%d,"pin":%d,"pin_id":%d,"mask":"%s","ues":[',
            ts, frame, slot, pin_flag, pin_window_id, rb_mask)
    else
        string_buffer[idx] = string.format('{"ts":%d,"f":%d,"s":%d,"pin":%d,"pin_id":%d,"ues":[',
            ts, frame, slot, pin_flag, pin_window_id)
    end
    idx = idx + 1

    for i = 0, n_ues - 1 do
        local m = metrics[i]

        if i > 0 then
            string_buffer[idx] = ","
            idx = idx + 1
        end

        -- Compact UE record with all inputs and outputs
        string_buffer[idx] = string.format(
            '{"r":%d,"uid":%d,"t":%d,"qi":%d,"tsnr":%d,"pb":%d,"tp":%.2f,"bl":%.4f,"snr":%d,"cqi":%d,"rsrp":%d,"mcs_p":%d,"rbs_req":%d,"mcs_req":%d,"rbs_out":%d,"mcs_out":%d,"rb_st":%d,"bwp_st":%d,"bwp_sz":%d',
            m.rnti,
            m.uid,
            m.ue_type,
            tonumber(m.fiveQI),
            m.target_snrx10,
            m.pending_bytes,
            m.throughput,
            m.bler,
            m.pusch_snrx10,
            m.cqi,
            m.dl_rsrp,
            m.previous_mcs,
            m.required_rbs,
            m.required_mcs,
            m.allocated_rb,
            m.allocated_mcs,
            m.allocated_rb_start,
            m.bwp_start,
            m.bwp_size
        )
        idx = idx + 1

        -- Optionally include channel magnitude (compact array)
        if LOG_CHANNEL_MAG and include_channel_mag then
            string_buffer[idx] = ',"cm":['
            idx = idx + 1
            string_buffer[idx] = format_channel_mag(m)
            idx = idx + 1
            string_buffer[idx] = ']'
            idx = idx + 1
        end

        string_buffer[idx] = '}'
        idx = idx + 1
    end

    string_buffer[idx] = "]}\n"

    -- Single non-blocking write for entire slot (buffers internally if pipe is full)
    local line = table.concat(string_buffer)
    pipe_write_buffered(line)
end

-- BLER-based MCS adaptation (updates every 10 frames, like C-side)
local BLER_UPDATE_FRAMES = 10
local ue_mcs_state = {}  -- rnti -> {mcs, last_frame}

local function adapt_mcs_bler(metric, max_mcs)
    local rnti = tonumber(metric.rnti)
    local frame = metric.frame
    local state = ue_mcs_state[rnti]

    if not state then
        state = {mcs = math.min(metric.previous_mcs, max_mcs), last_frame = frame}
        ue_mcs_state[rnti] = state
        return state.mcs
    end

    local diff = frame - state.last_frame
    if diff < 0 then diff = diff + 1024 end  -- wrap around
    if diff < BLER_UPDATE_FRAMES then
        return state.mcs
    end

    state.last_frame = frame
    local bler = metric.bler

    if bler < BLER_TARGET_LOW and state.mcs < max_mcs then
        state.mcs = state.mcs + 1
    elseif bler > BLER_TARGET_HIGH and state.mcs > 0 then
        state.mcs = state.mcs - 1
    end

    return state.mcs
end

-- RB allocation helpers
local function find_contiguous_free_rbs_for_ue(rb_mask_string, start_pos, required_rbs, metric)
    local bwp_start = metric.bwp_start
    local bwp_size = metric.bwp_size

    if start_pos + required_rbs > bwp_size then return -1 end

    for i = start_pos, start_pos + required_rbs - 1 do
        local mask_index = bwp_start + i + 1
        if rb_mask_string:sub(mask_index, mask_index) == 'X' then
            return -1
        end
        -- per-UE PRB blocking check
        if metric.ul_prb_mask[bwp_start + i] == string.byte('X') then
            return -1
        end
    end
    return start_pos
end

local function find_first_contiguous_rbs_for_ue(rb_mask_string, required_rbs, metric)
    local bwp_size = metric.bwp_size
    for start_pos = 0, bwp_size - required_rbs do
        if find_contiguous_free_rbs_for_ue(rb_mask_string, start_pos, required_rbs, metric) >= 0 then
            return start_pos
        end
    end
    return -1
end

local function find_largest_free_block_for_ue(rb_mask_string, metric)
    local bwp_start = metric.bwp_start
    local bwp_size = metric.bwp_size
    local max_size = 0
    local best_start = 0
    local current_start = -1
    local current_size = 0

    for rb = 0, bwp_size - 1 do
        local mask_index = bwp_start + rb + 1
        local global_free = rb_mask_string:sub(mask_index, mask_index) == '.'
        local ue_free = metric.ul_prb_mask[bwp_start + rb] ~= string.byte('X')
        if global_free and ue_free then
            if current_start < 0 then
                current_start = rb
                current_size = 1
            else
                current_size = current_size + 1
            end
        else
            if current_size > max_size then
                max_size = current_size
                best_start = current_start
            end
            current_start = -1
            current_size = 0
        end
    end
    if current_size > max_size then
        max_size = current_size
        best_start = current_start
    end
    return best_start, max_size
end

local function mark_rbs_used(rb_mask_string, start_rb, num_rbs, bwp_start)
    local mask_chars = {}
    for i = 1, #rb_mask_string do
        mask_chars[i] = rb_mask_string:sub(i, i)
    end
    for rb = start_rb, start_rb + num_rbs - 1 do
        mask_chars[bwp_start + rb + 1] = 'X'
    end
    return table.concat(mask_chars)
end

-- Phase allocators (used only during PF recompute)
local function allocate_harq_retransmissions(metrics, n_ues, rb_mask_string, min_rbs)
    local current_mask = rb_mask_string

    for i = 0, n_ues - 1 do
        local m = metrics[i]
        if m.ue_type == UE_TYPE_HARQ_RETX then
            local required = m.required_rbs
            if required > 0 then
                local start_rb = find_first_contiguous_rbs_for_ue(current_mask, required, m)
                if start_rb >= 0 then
                    m.allocated_rb = required
                    m.allocated_mcs = m.required_mcs
                    m.allocated_rb_start = start_rb
                    current_mask = mark_rbs_used(current_mask, start_rb, required, m.bwp_start)
                end
            end
        end
    end
    return current_mask
end

local function allocate_no_data_ues(metrics, n_ues, rb_mask_string, min_rbs)
    local current_mask = rb_mask_string

    for i = 0, n_ues - 1 do
        local m = metrics[i]
        if m.ue_type == UE_TYPE_NO_DATA then
            local required = m.required_rbs
            if required > 0 then
                local start_rb = find_first_contiguous_rbs_for_ue(current_mask, required, m)
                if start_rb >= 0 then
                    m.allocated_rb = required
                    m.allocated_mcs = m.required_mcs
                    m.allocated_rb_start = start_rb
                    current_mask = mark_rbs_used(current_mask, start_rb, required, m.bwp_start)
                end
            end
        end
    end
    return current_mask
end

-- Get per-class throughput limit (Mbps) from C-set Lua globals
-- Returns limit in bytes/s, or nil if no limit
-- Classification: fiveQI == 9 → FWA (broadband), otherwise → MTC
local function get_throughput_limit_bps(fiveQI)
    local limit_mbps
    if fiveQI == 9 then
        limit_mbps = fwa_max_throughput  -- global set by C via set_lua_ul_scheduler_config()
    else
        limit_mbps = mtc_max_throughput
    end
    if limit_mbps and limit_mbps > 0 then
        return limit_mbps * 1e6 / 8  -- Mbps → bytes/s
    end
    return nil
end

-- Token bucket rate limiter with adaptive calibration.
-- NR PUSCH MCS Table 1 spectral efficiency (bits/RE) from 3GPP TS 38.214
local mcs_se = {
    [0]=0.23, 0.31, 0.38, 0.49, 0.60, 0.74, 0.88, 1.03, 1.18, 1.33,
    1.33, 1.48, 1.70, 1.91, 2.16, 2.41, 2.57, 2.57, 2.73, 3.03,
    3.32, 3.61, 3.90, 4.21, 4.52, 4.82, 5.12, 5.33
}
local DATA_RE_PER_RB = 12 * 12  -- 12 subcarriers × 12 data symbols

-- Per-UE token bucket and calibration state
local ue_bucket = {}       -- per-RNTI: {budget, last_time, scale, est_sum, tp_sum}
local BURST_WINDOW = 0.05  -- max 50ms worth of accumulated budget
local INITIAL_SCALE = 0.7  -- initial bytes_per_rb scale guess
local CAL_WINDOW = 0.5     -- calibrate every 500ms of accumulated data
local CAL_ALPHA = 0.3      -- how fast to adopt new calibration (0=ignore, 1=instant)

local function allocate_new_data_ues(metrics, n_ues, rb_mask_string, min_rbs)
    local current_mask = rb_mask_string
    local now = socket.gettime()

    -- Collect new data UEs and compute PF metrics
    local new_data_ues = {}
    for i = 0, n_ues - 1 do
        local m = metrics[i]
        if m.ue_type == UE_TYPE_NEW_DATA then
            local tp = m.throughput
            if tp < 1.0 then tp = 1.0 end
            local pf_metric = m.pending_bytes / tp
            table.insert(new_data_ues, {index = i, metric = m, pf = pf_metric})
        end
    end

    -- Sort by PF metric (descending)
    table.sort(new_data_ues, function(a, b) return a.pf > b.pf end)

    -- Allocate in PF order, capping RBs via token bucket
    for _, ue in ipairs(new_data_ues) do
        local m = ue.metric
        local best_start, max_free = find_largest_free_block_for_ue(current_mask, m)

        if max_free >= min_rbs then
            local mcs = math.min(m.previous_mcs, 27)
            local alloc_rbs = math.min(max_free, 51)

            -- Token bucket rate limiting
            local limit = get_throughput_limit_bps(tonumber(m.fiveQI))
            if limit then
                local rnti = tonumber(m.rnti)
                local b = ue_bucket[rnti]
                if not b then
                    b = {budget = limit * BURST_WINDOW, last_time = now,
                         scale = INITIAL_SCALE, est_sum = 0, tp_sum = 0, cal_time = now}
                    ue_bucket[rnti] = b
                end

                -- Replenish: add tokens at target rate
                local dt = now - b.last_time
                if dt > 0 then
                    b.budget = b.budget + limit * dt
                    b.budget = math.min(b.budget, limit * BURST_WINDOW)
                    b.last_time = now
                end

                -- Adaptive calibration
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

                -- Compute scaled bytes per RB
                local se = mcs_se[mcs] or 2.0
                local bytes_per_rb = m.nr_of_layers * se * DATA_RE_PER_RB / 8 * b.scale

                -- Cap RBs to fit within budget
                if bytes_per_rb > 0 then
                    local max_rbs = math.floor(b.budget / bytes_per_rb)
                    alloc_rbs = math.min(alloc_rbs, math.max(min_rbs, max_rbs))
                end

                -- Deduct from budget and track for calibration
                local deducted = alloc_rbs * bytes_per_rb
                b.budget = b.budget - deducted
                b.est_sum = b.est_sum + deducted
            end

            m.allocated_rb = alloc_rbs
            m.allocated_mcs = mcs
            m.allocated_rb_start = best_start
            current_mask = mark_rbs_used(current_mask, best_start, alloc_rbs, m.bwp_start)
        end
    end

    return current_mask
end

-- Main allocation function with pinned allocation support
function compute_allocations(metrics_ptr, n_ues, total_rbs, min_rbs, rb_mask_string)
    local metrics = ffi.cast("ue_metric_ul_t*", metrics_ptr)

    -- Get frame/slot from first UE
    local frame = 0
    local slot = 0
    if n_ues > 0 then
        frame = metrics[0].frame
        slot = metrics[0].slot
    end

    local current_mask = rb_mask_string

    pin_counter = pin_counter + 1
    local recompute = (pin_counter >= PIN_INTERVAL_SLOTS)

    if recompute then
        -- ============ PF RECOMPUTE: full scheduling, cache results ============
        pin_counter = 0
        pin_window_id = pin_window_id + 1
        pinned_allocs = {}

        -- Run all phases normally
        current_mask = allocate_harq_retransmissions(metrics, n_ues, current_mask, min_rbs)
        current_mask = allocate_no_data_ues(metrics, n_ues, current_mask, min_rbs)
        current_mask = allocate_new_data_ues(metrics, n_ues, current_mask, min_rbs)

        -- Cache all allocations (regardless of UE type)
        for i = 0, n_ues - 1 do
            local m = metrics[i]
            if m.allocated_rb > 0 then
                pinned_allocs[tonumber(m.rnti)] = {
                    rb_start = m.allocated_rb_start,
                    num_rbs  = m.allocated_rb,
                    mcs      = m.allocated_mcs,
                }
            end
        end
    else
        -- ============ REPLAY: use pinned allocations ============
        for i = 0, n_ues - 1 do
            local m = metrics[i]
            local pin = pinned_allocs[tonumber(m.rnti)]
            if pin then
                local rbs = pin.num_rbs
                -- always use C's latest MCS (tracks BLER adaptation)
                local mcs = math.min(m.previous_mcs, 27)

                -- HARQ retx may need different RB count/MCS, respect it
                -- but keep the same start position
                if m.ue_type == UE_TYPE_HARQ_RETX then
                    rbs = m.required_rbs
                    mcs = m.required_mcs
                end

                local ok = find_contiguous_free_rbs_for_ue(
                    current_mask, pin.rb_start, rbs, m)
                if ok >= 0 then
                    m.allocated_rb = rbs
                    m.allocated_mcs = mcs
                    m.allocated_rb_start = pin.rb_start
                    current_mask = mark_rbs_used(
                        current_mask, pin.rb_start, rbs, m.bwp_start)
                end
                -- If PRBs unavailable this slot, skip — don't allocate elsewhere
            end
            -- UEs without a pinned entry get nothing
        end
    end

    -- Log metrics (after allocation decisions are made)
    slot_counter = slot_counter + 1
    if slot_counter >= LOG_EVERY_N_SLOTS then
        slot_counter = 0
        local ts = get_timestamp_ns()
        log_metrics_batched(metrics, n_ues, frame, slot, ts, false, rb_mask_string, recompute)
    end

    -- Retry pipe if disconnected
    maybe_retry_pipe()

    return current_mask
end
