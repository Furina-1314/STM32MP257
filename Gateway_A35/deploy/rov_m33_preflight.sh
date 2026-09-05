#!/bin/sh
# rov_m33_preflight.sh - ensure the ROV M33 firmware is running before the
# gateway starts. Only reloads firmware when the remoteproc is not running
# the expected ROV image: a gateway restart must NOT reboot the M33 (that
# would clear the stop latches).
RPROC=/sys/class/remoteproc/remoteproc0
FW_NAME=ROV_M33_CM33_NonSecure.elf
FW_SCRIPT=/home/root/ROV_M33/lib/fw_cortex_m33.sh

STATE="$(cat ${RPROC}/state 2>/dev/null)"
CUR_FW="$(cat ${RPROC}/firmware 2>/dev/null)"

if [ "$STATE" = "running" ] && [ "$CUR_FW" = "$FW_NAME" ]; then
    echo "rov_m33_preflight: ${FW_NAME} already running"
    exit 0
fi

echo "rov_m33_preflight: state=${STATE} firmware=${CUR_FW}; (re)loading ROV firmware"
exec "$FW_SCRIPT" start
