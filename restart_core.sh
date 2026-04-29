#!/bin/bash
# Full core network restart sequence:
#   1. Stop the OAI restart loop (not just nr-softmodem, the whole loop)
#   2. Stop the UL control API loop
#   3. Restart the 5G core (oc delete pods --all, blocks until ready)
#   4. Re-inject the OAI and control API loops into their tmux panes
#
# Usage:  ./restart_core.sh

set -e

SESSION="mwc-demo"
OAI_PANE="${SESSION}:0.1"
CTL_PANE="${SESSION}:0.2"
DIR="/phy/openairinterface5g"
CONF_RUNTIME="${DIR}/demo_runtime.conf"
CORE_NS="open5gs-001-09"
OC="/usr/local/bin/oc"

LOCKFILE="/tmp/restart_core.lock"

if ! tmux has-session -t "$SESSION" 2>/dev/null; then
    echo "[core-restart] ERROR: tmux session '$SESSION' not found"
    exit 1
fi

# Create lockfile to prevent other watchdogs from interfering
trap 'rm -f "$LOCKFILE"' EXIT
touch "$LOCKFILE"

# --- Step 1: Stop OAI loop ---
# Spam Ctrl+C to reliably kill both nr-softmodem and the while-true loop
echo "[core-restart] Stopping OAI loop ($OAI_PANE) ..."
for i in $(seq 50); do
    tmux send-keys -t "$OAI_PANE" C-c
    sleep 0.1
done

# --- Step 2: Stop UL control API loop ---
echo "[core-restart] Stopping UL control API loop ($CTL_PANE) ..."
for i in $(seq 50); do
    tmux send-keys -t "$CTL_PANE" C-c
    sleep 0.1
done

# Make sure processes are really dead
pkill -x nr-softmodem 2>/dev/null || true
pkill -f UL_control_termination 2>/dev/null || true
sleep 1

echo "[core-restart] OAI fully stopped."

# --- Step 3: Restart the core network ---
echo "[core-restart] Restarting 5G core (namespace: $CORE_NS) ..."
"$OC" -n "$CORE_NS" delete pods --all
echo "[core-restart] Waiting for core pods to be ready ..."
"$OC" -n "$CORE_NS" wait --for=condition=Ready pods --all --timeout=120s
echo "[core-restart] Core network is up."

# --- Step 4: Re-inject the loops ---
echo "[core-restart] Restarting OAI loop ..."
tmux send-keys -t "$OAI_PANE" \
    "export LUA_SCHED_UL=${DIR}/pf_ul_pinned_logger.lua; while true; do echo '>>> Starting OAI...'; ${DIR}/cmake_targets/ran_build/build/nr-softmodem -O ${CONF_RUNTIME} --telnetsrv; echo '>>> OAI exited, restarting in 1s...'; pkill -f UL_control_termination 2>/dev/null; sleep 1; done" Enter

sleep 5

echo "[core-restart] Restarting UL control API loop ..."
tmux send-keys -t "$CTL_PANE" \
    "cd ${DIR}; while true; do python3 UL_control_termination.py; sleep 1; done" Enter

echo "[core-restart] Done. Full restart complete."
