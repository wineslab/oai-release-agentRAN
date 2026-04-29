#!/bin/bash
# Watchdog: monitors OAI tmux pane for core network failures (NGAP errors)
# and triggers restart_core.sh when detected.
#
# Usage:
#   ./watchdog_core.sh              # live mode
#   ./watchdog_core.sh --dry-run    # detect but don't restart
#
# Detection: greps the OAI pane scrollback for NGAP error patterns.
# After triggering a restart, clears the pane scrollback to avoid re-triggering.

DIR="$(cd "$(dirname "$0")" && pwd)"
SESSION="mwc-demo"
OAI_PANE="${SESSION}:0.1"
LOCKFILE="/tmp/restart_core.lock"
CHECK_INTERVAL=1  # seconds between checks
NGAP_PATTERN="NGAP_CauseRadioNetwork_unknown_local_UE_NGAP_ID"

DRY_RUN=0
if [ "$1" = "--dry-run" ]; then
    DRY_RUN=1
    echo "[watchdog-core] DRY-RUN mode: will detect but NOT restart"
fi

echo "[watchdog-core] Monitoring pane $OAI_PANE for NGAP errors (every ${CHECK_INTERVAL}s)"

while true; do
    sleep "$CHECK_INTERVAL"

    # Skip if tmux session doesn't exist
    if ! tmux has-session -t "$SESSION" 2>/dev/null; then
        continue
    fi

    # Skip if a core restart is already in progress
    if [ -f "$LOCKFILE" ]; then
        echo "[watchdog-core] Core restart in progress (lockfile exists), skipping check"
        continue
    fi

    # Capture the OAI pane scrollback
    pane_content=$(tmux capture-pane -t "$OAI_PANE" -p -S -1000 2>/dev/null)

    # Check for NGAP error pattern
    if echo "$pane_content" | grep -q "$NGAP_PATTERN"; then
        echo "[watchdog-core] NGAP error detected!"

        if [ "$DRY_RUN" -eq 1 ]; then
            echo "[watchdog-core] DRY-RUN: would run restart_core.sh"
            # Clear scrollback even in dry-run so we don't spam detections
            tmux clear-history -t "$OAI_PANE" 2>/dev/null
            echo "[watchdog-core] Cleared OAI pane scrollback"
        else
            echo "[watchdog-core] Triggering core restart..."
            "${DIR}/restart_core.sh"
            # Clear scrollback after restart so we don't re-trigger on stale logs
            tmux clear-history -t "$OAI_PANE" 2>/dev/null
            echo "[watchdog-core] Cleared OAI pane scrollback"
        fi
    fi
done
