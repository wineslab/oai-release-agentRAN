#!/bin/bash
# Send Ctrl+C to the OAI pane and then the UL Control API pane
# in the mwc-demo tmux session.
# Both panes run restart loops, so they will automatically relaunch.
#
# Usage:  ./restart_oai.sh

SESSION="mwc-demo"
OAI_PANE="${SESSION}:0.1"
CTL_PANE="${SESSION}:0.2"
CTL_DELAY=5  # seconds to wait before restarting control API

LOCKFILE="/tmp/restart_core.lock"

if [ -f "$LOCKFILE" ]; then
    echo "[restart] Core restart in progress (lockfile exists), skipping"
    exit 0
fi

if ! tmux has-session -t "$SESSION" 2>/dev/null; then
    echo "[restart] ERROR: tmux session '$SESSION' not found"
    exit 1
fi

echo "[restart] Sending Ctrl+C to OAI ($OAI_PANE) ..."
tmux send-keys -t "$OAI_PANE" C-c

echo "[restart] Waiting ${CTL_DELAY}s for OAI to come back up ..."
sleep "$CTL_DELAY"

echo "[restart] Sending Ctrl+C to UL Control API ($CTL_PANE) ..."
tmux send-keys -t "$CTL_PANE" C-c

echo "[restart] Done. Both loops should relaunch."
