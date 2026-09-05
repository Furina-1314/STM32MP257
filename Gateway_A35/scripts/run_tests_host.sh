#!/usr/bin/env bash
# Run all host tests and the gateway self-check; nonzero exit on any failure.
# Each suite is bounded by timeout so a hanging test cannot stall CI.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=build_host
TESTS="crc16 wire_codec frame_accumulator function_registry payload_codec gateway_state command_queue gateway_core tcp_server dht11_reader ina226_reader sensor_service ini_config"

run_bounded() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 120 "$@"
    else
        "$@"
    fi
}

pass=0
fail=0
for t in $TESTS; do
    if run_bounded "$OUT/test_${t}"; then
        pass=$((pass + 1))
    else
        echo "TEST FAILED (or timed out): ${t}"
        fail=$((fail + 1))
    fi
done

if run_bounded "$OUT/rov_gateway" --check; then
    pass=$((pass + 1))
else
    echo "SELF-CHECK FAILED"
    fail=$((fail + 1))
fi

echo "----------------------------------------"
echo "suites passed: ${pass}, failed: ${fail}"
[ "$fail" -eq 0 ]
