#!/bin/sh
# wait_rpmsg.sh - bounded wait for the RPMsg endpoint after M33 start.
DEV=/dev/ttyRPMSG0
TIMEOUT_SECS=15
i=0
while [ $i -lt $TIMEOUT_SECS ]; do
    if [ -e "$DEV" ]; then
        echo "wait_rpmsg: $DEV present"
        exit 0
    fi
    sleep 1
    i=$((i + 1))
done
echo "wait_rpmsg: $DEV not present after ${TIMEOUT_SECS}s" >&2
exit 1
