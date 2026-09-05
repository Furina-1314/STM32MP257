#!/bin/sh
# rov_m33_gate.sh - self-test gate with one-shot M33 recovery (T-01).
# Runs as the last ExecStartPre of rov_gateway.service:
#   - self-test PASS -> gateway starts against a healthy M33 (normal path);
#   - self-test FAIL -> the M33 is wedged (data queries time out / rpmsg tx
#     pool exhausted). Restart the M33 firmware once, then re-run the
#     self-test: its final step re-latches the global stop, so outputs stay
#     disabled when the gateway comes up. A second failure is a real fault
#     (systemd rate limiting then guards the loop).
SELF_TEST=/home/root/rov_control/build/rov_self_test
FW_SCRIPT=/home/root/ROV_M33/lib/fw_cortex_m33.sh

if "$SELF_TEST"; then
    exit 0
fi

echo "rov_m33_gate: self-test failed - restarting M33 (T-01 recovery)" >&2
if ! "$FW_SCRIPT" start; then
    echo "rov_m33_gate: M33 firmware restart failed" >&2
    exit 1
fi
sleep 2
exec "$SELF_TEST"
