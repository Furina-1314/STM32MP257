#!/usr/bin/env bash
# Host build without CMake (Windows/MinGW g++ or any C++17 compiler).
# The board build uses CMakeLists.txt; keep both in sync when files are added.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--std=c++17 -Wall -Wextra -O2}"
# (host build does not compile the vendor RPMsg stack; board CMake does)
OUT=build_host
mkdir -p "$OUT"

WIRE_SRC="src/wire/crc16.cpp src/wire/wire_codec.cpp src/wire/frame_accumulator.cpp src/wire/function_registry.cpp src/wire/payload_codec.cpp"
CORE_SRC="src/core/gateway_state.cpp src/core/command_queue.cpp src/core/gateway_core.cpp src/net/net_platform.cpp src/net/tcp_server.cpp src/util/log.cpp src/util/ini_config.cpp src/sensors/file_io.cpp src/sensors/dht11_reader.cpp src/sensors/ina226_reader.cpp src/sensors/m33_sensor_reader.cpp src/sensors/sensor_service.cpp"
ALL_SRC="$WIRE_SRC $CORE_SRC"

LIBS=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows*) LIBS="-lws2_32" ;;
esac

TESTS="crc16 wire_codec frame_accumulator function_registry payload_codec gateway_state command_queue gateway_core tcp_server dht11_reader ina226_reader sensor_service ini_config"

for t in $TESTS; do
    # shellcheck disable=SC2086
    "$CXX" $CXXFLAGS -Isrc -Itests -Ivendor/rov_control/include "tests/test_${t}.cpp" $ALL_SRC $LIBS -o "$OUT/test_${t}"
done

# shellcheck disable=SC2086
"$CXX" $CXXFLAGS -Isrc -Ivendor/rov_control/include src/main.cpp $ALL_SRC $LIBS -o "$OUT/rov_gateway"

echo "host build ok: $OUT"
