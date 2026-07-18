#!/usr/bin/env bash
# OTA push + verification for the viewport firmware.
#
# The bootloader reverts to the previous slot unless the new image reaches
# ota_state=valid (the firmware's 30 s healthy timer marks it). Known quirk:
# the FIRST push of a new binary sometimes silently rolls back — the
# definitive tell is /state reading "valid" at low uptime instead of
# "pending-verify". `push` detects that and re-pushes once automatically.
#
# usage: tools/ota.sh push   <host> [bin]     # push + verify (+1 auto-retry)
#        tools/ota.sh verify <host>           # post-push verification only
set -euo pipefail

cmd="${1:?usage: tools/ota.sh {push|verify} <host> [bin]}"
host="${2:?usage: tools/ota.sh {push|verify} <host> [bin]}"
bin="${3:-build/scrypted-viewport.bin}"

state_line() {
    curl -sS --max-time 3 "http://$host/state" 2>/dev/null |
        python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["ota_state"], d["uptime_ms"], d["version"])' \
            2>/dev/null || true
}

# Poll /state through the reboot. Success = pending-verify observed on a
# fresh boot, then valid (healthy timer fired). Failure = valid with
# pending-verify never seen — the device booted the OLD slot (or we
# started polling too late to tell; treated as rollback to be safe).
verify() {
    local saw_pending=0 i ota up ver
    for i in $(seq 1 25); do
        sleep 4
        # shellcheck disable=SC2046
        set -- $(state_line)
        if [ $# -lt 3 ]; then continue; fi        # rebooting / unreachable
        ota=$1; up=$2; ver=$3
        echo "  t+$((i * 4))s: $ota uptime=${up}ms v$ver"
        if [ "$ota" = "pending-verify" ]; then
            saw_pending=1
        elif [ "$ota" = "valid" ]; then
            if [ "$saw_pending" = 1 ]; then
                echo "OK: new image verified (pending-verify -> valid, v$ver)"
                return 0
            fi
            echo "ROLLBACK: valid at ${up}ms uptime, pending-verify never seen — old slot is running"
            return 1
        fi
    done
    echo "TIMEOUT: never reached valid"
    return 1
}

push() {
    echo "pushing $bin -> http://$host/firmware"
    curl -sS --max-time 60 -X POST --data-binary @"$bin" "http://$host/firmware"
    echo
}

case "$cmd" in
    push)
        push
        if ! verify; then
            echo "re-pushing (known first-push rollback quirk)"
            push
            verify || { echo "FAILED: rolled back again after retry"; exit 1; }
        fi
        ;;
    verify)
        verify
        ;;
    *)
        echo "usage: tools/ota.sh {push|verify} <host> [bin]" >&2
        exit 2
        ;;
esac
