#!/bin/bash
# ============================================================
# MWC Demo Launcher
# ============================================================
# Setup:
#   1. Detects eth0 IP, net1 bus-info, net1 MAC, CPU cores
#   2. Patches demo.conf and RU_conf.xml with runtime values
#   3. Uploads RU config, resets RU
#   4. Starts UEs once (connection manager + iperf loop)
#   5. Creates tmux "mwc-demo" with 4 panes:
#      - Forwarder, OAI (auto-restart loop), Control API, Core Watchdog
#
# Usage:
#   ./demo.sh                    # setup + launch
#   ./demo.sh --watchdog-dry-run # launch with core watchdog in dry-run mode
#   tmux attach -t mwc-demo      # view panes
# ============================================================

set -e

# Parse arguments
WATCHDOG_DRY_RUN=""
for arg in "$@"; do
    case "$arg" in
        --watchdog-dry-run) WATCHDOG_DRY_RUN="--dry-run" ;;
    esac
done

SESSION="mwc-demo"
DIR="/phy/openairinterface5g"
CONF_TEMPLATE="${DIR}/demo.conf"
CONF_RUNTIME="${DIR}/demo_runtime.conf"
RU_TEMPLATE="${DIR}/RU_conf.xml"
RU_RUNTIME="${DIR}/RU_conf_runtime.xml"
RU_HOST="fxn09.x5g.open6g.net"
RU_PORT=830
RU_USER="root"
RU_PASS="wneslmwc"

UE_USER="sierra"
UE_PASS="wnesl"
UE_TMUX="ue-demo"
MBIM_CMD="sudo /home/sierra/sierra_utilities/sdk/SampleApps/lite-mbim-connection-manager/bin/lite-mbim-connection-managerhostx86_64 -s /usr/local/settings.conf"

# --- Kill any leftovers from a previous run ---
echo "[setup] Cleaning up previous run..."
pkill -x nr-softmodem 2>/dev/null || true
pkill -f ul_metrics_forwarder 2>/dev/null || true
pkill -f UL_control_termination 2>/dev/null || true
pkill -f watchdog_core 2>/dev/null || true
tmux kill-session -t "$SESSION" 2>/dev/null || true
sleep 1

# --- Install pip dependencies if missing ---
pip3 install --quiet requests aiohttp fastapi uvicorn pexpect 2>/dev/null

# ============================================================
# PHASE 1: Detect runtime parameters
# ============================================================
echo "[setup] Detecting runtime parameters..."

ETH0_IP=$(ip -4 addr show eth0 | grep -oP 'inet \K[0-9.]+')
if [ -z "$ETH0_IP" ]; then echo "[setup] ERROR: Could not detect eth0 IP"; exit 1; fi
echo "[setup]   eth0 IP:      $ETH0_IP"

NET1_BUS=$(ethtool -i net1 | grep bus-info | awk '{print $2}')
if [ -z "$NET1_BUS" ]; then echo "[setup] ERROR: Could not detect net1 bus-info"; exit 1; fi
echo "[setup]   net1 bus:     $NET1_BUS"

NET1_MAC=$(ip link show net1 | grep 'link/ether' | awk '{print $2}')
if [ -z "$NET1_MAC" ]; then echo "[setup] ERROR: Could not detect net1 MAC"; exit 1; fi
echo "[setup]   net1 MAC:     $NET1_MAC"

# Expand taskset output (e.g. "0-3,25,27,29,31,33") into individual core numbers
CORES_RAW=$(taskset -cp 1 | grep -oP 'current affinity list: \K.*')
CORE_ARRAY=()
IFS=',' read -ra SEGMENTS <<< "$CORES_RAW"
for seg in "${SEGMENTS[@]}"; do
    if [[ "$seg" == *-* ]]; then
        IFS='-' read -r lo hi <<< "$seg"
        for ((c=lo; c<=hi; c++)); do CORE_ARRAY+=("$c"); done
    else
        CORE_ARRAY+=("$seg")
    fi
done
if [ "${#CORE_ARRAY[@]}" -lt 6 ]; then
    echo "[setup] ERROR: Need at least 6 cores, got ${#CORE_ARRAY[@]}: $CORES_RAW"; exit 1
fi
CORE_1=${CORE_ARRAY[0]}; CORE_2=${CORE_ARRAY[1]}; CORE_3=${CORE_ARRAY[2]}
CORE_4=${CORE_ARRAY[3]}; CORE_5=${CORE_ARRAY[4]}; CORE_6=${CORE_ARRAY[5]}
echo "[setup]   Cores:        $CORE_1, $CORE_2, $CORE_3, $CORE_4, $CORE_5, $CORE_6"

# ============================================================
# PHASE 2: Patch config files
# ============================================================
echo "[setup] Patching demo.conf..."
cp "$CONF_TEMPLATE" "$CONF_RUNTIME"
sed -i "s/PLACEHOLDER_1/${ETH0_IP}/g"   "$CONF_RUNTIME"
sed -i "s/PLACEHOLDER_2/${NET1_BUS}/g"  "$CONF_RUNTIME"
sed -i "s/CORE_6/${CORE_6}/g"           "$CONF_RUNTIME"
sed -i "s/CORE_5/${CORE_5}/g"           "$CONF_RUNTIME"
sed -i "s/CORE_4/${CORE_4}/g"           "$CONF_RUNTIME"
sed -i "s/CORE_3/${CORE_3}/g"           "$CONF_RUNTIME"
sed -i "s/CORE_2/${CORE_2}/g"           "$CONF_RUNTIME"
sed -i "s/CORE_1/${CORE_1}/g"           "$CONF_RUNTIME"
echo "[setup]   -> $CONF_RUNTIME"

echo "[setup] Patching RU_conf.xml..."
cp "$RU_TEMPLATE" "$RU_RUNTIME"
sed -i "s/PLACEHOLDER_MAC/${NET1_MAC}/g" "$RU_RUNTIME"
echo "[setup]   -> $RU_RUNTIME"

# ============================================================
# PHASE 3: Upload RU config and reset RU
# ============================================================
echo "[setup] Uploading RU config to ${RU_HOST}..."
sshpass -p "$RU_PASS" scp -P "$RU_PORT" -o StrictHostKeyChecking=no \
    "$RU_RUNTIME" "${RU_USER}@${RU_HOST}:/home/root/test/RRHconfig_xran.xml"

echo "[setup] Resetting RU CU-plane..."
sshpass -p "$RU_PASS" ssh -p "$RU_PORT" -o StrictHostKeyChecking=no \
    "${RU_USER}@${RU_HOST}" \
    "cd /home/root/test && ./reset_cuplane && sleep 2 && ./init_rrh_config_enable_cuplane"
echo "[setup] RU ready."

# ============================================================
# PHASE 4: Start UEs once (persist across OAI restarts)
# ============================================================
start_ue() {
    local host="$1"
    echo "[setup] Setting up UE (${UE_USER}@${host})..."
    sshpass -p "$UE_PASS" ssh -o StrictHostKeyChecking=no \
        "${UE_USER}@${host}" "tmux kill-session -t ${UE_TMUX} 2>/dev/null; true"

    sshpass -p "$UE_PASS" ssh -o StrictHostKeyChecking=no \
        "${UE_USER}@${host}" \
        "tmux new-session -d -s ${UE_TMUX} && tmux split-window -v -t ${UE_TMUX}"

    sshpass -p "$UE_PASS" ssh -o StrictHostKeyChecking=no \
        "${UE_USER}@${host}" \
        "tmux send-keys -t ${UE_TMUX}:0.0 'while true; do ${MBIM_CMD}; sleep 2; done' Enter"
    sleep 1
    sshpass -p "$UE_PASS" ssh -o StrictHostKeyChecking=no \
        "${UE_USER}@${host}" \
        "tmux send-keys -t ${UE_TMUX}:0.0 '${UE_PASS}' Enter"

    sshpass -p "$UE_PASS" ssh -o StrictHostKeyChecking=no \
        "${UE_USER}@${host}" \
        "tmux send-keys -t ${UE_TMUX}:0.1 'while true; do iperf -c 10.45.0.1 -u -b 50M -t 0 -i 1; sleep 2; done' Enter"

    echo "[setup] UE started (tmux session: ${UE_TMUX} on ${host})"
}

start_ue "10.112.1.92"
start_ue "10.112.1.80"
start_ue "10.112.1.103"

# ============================================================
# PHASE 5: Create tmux session with 3 panes
# ============================================================
tmux new-session -d -s "$SESSION"
tmux split-window -v -t "$SESSION"
tmux split-window -v -t "$SESSION"
tmux split-window -v -t "$SESSION"
tmux select-layout -t "$SESSION" even-vertical
tmux set -t "$SESSION" pane-border-status top 2>/dev/null
tmux select-pane -t "$SESSION:0.0" -T "Metrics Forwarder" 2>/dev/null
tmux select-pane -t "$SESSION:0.1" -T "OAI gNB" 2>/dev/null
tmux select-pane -t "$SESSION:0.2" -T "UL Control API" 2>/dev/null
tmux select-pane -t "$SESSION:0.3" -T "Core Watchdog" 2>/dev/null

# Pane 0: Metrics forwarder
tmux send-keys -t "$SESSION:0.0" \
    "python3 ${DIR}/ul_metrics_forwarder.py -e live" Enter

# Pane 1: OAI gNB + Control API in a restart loop
tmux send-keys -t "$SESSION:0.1" \
    "export LUA_SCHED_UL=${DIR}/pf_ul_pipe_logger.lua; while true; do echo '>>> Starting OAI...'; ${DIR}/cmake_targets/ran_build/build/nr-softmodem -O ${CONF_RUNTIME} --telnetsrv; echo '>>> OAI exited, restarting in 1s...'; pkill -f UL_control_termination 2>/dev/null; sleep 1; done" Enter

# Pane 2: UL Control API in a restart loop (follows OAI)
tmux send-keys -t "$SESSION:0.2" \
    "cd ${DIR}; while true; do python3 UL_control_termination.py; sleep 1; done" Enter

# Pane 3: Core network watchdog
# tmux send-keys -t "$SESSION:0.3" \
#     "${DIR}/watchdog_core.sh ${WATCHDOG_DRY_RUN}" Enter

echo ""
echo "============================================"
echo " Demo launched!"
echo " tmux attach -t $SESSION"
echo "============================================"
