cmake_minimum_required(VERSION 3.16)
project(rov_gateway CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# Wire layer: Windows<->A35 protocol (frame codec, CRC16, accumulator,
# 41-entry function registry, typed payload codecs). Pure C++17, no
# third-party dependencies, buildable natively on the board.
add_library(gateway_wire STATIC
    src/wire/crc16.cpp
    src/wire/wire_codec.cpp
    src/wire/frame_accumulator.cpp
    src/wire/function_registry.cpp
    src/wire/payload_codec.cpp
)
target_include_directories(gateway_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Core + transport: state authority, command dispatch/priority queue, M33
# seam (IRovControl), single-client TCP server (WinSock/BSD portable).
add_library(gateway_core STATIC
    src/core/gateway_state.cpp
    src/core/command_queue.cpp
    src/core/gateway_core.cpp
    src/net/net_platform.cpp
    src/net/tcp_server.cpp
    src/util/log.cpp
    src/util/ini_config.cpp
    src/sensors/file_io.cpp
    src/sensors/dht11_reader.cpp
    src/sensors/ina226_reader.cpp
    src/sensors/m33_sensor_reader.cpp
    src/sensors/sensor_service.cpp
)
target_include_directories(gateway_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/rov_control/include
)
target_link_libraries(gateway_core PUBLIC gateway_wire)

find_package(Threads REQUIRED)
target_link_libraries(gateway_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(gateway_core PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# Probe tool (portable; used for board verification in phase 4+).
add_executable(gateway_probe tools/gateway_probe.cpp)
target_link_libraries(gateway_probe PRIVATE gateway_core)

# ---------------------------------------------------------------------------
# Main executable.
#   - board (Linux): full gateway over the real RovControl/RPMsg stack
#   - host (Windows): skeleton with the wire self-check
if(UNIX)
    add_library(rov_control_lib STATIC
        vendor/rov_control/src/rov_control.cpp
        vendor/rov_control/src/internal/rpmsg_client.cpp
        vendor/rov_control/src/internal/response_parser.cpp
        vendor/rov_control/src/internal/transport.cpp
        vendor/rov_control/src/internal/posix_tty_transport.cpp
    )
    target_include_directories(rov_control_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/rov_control/include
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/rov_control/src)
    target_compile_options(rov_control_lib PRIVATE -Wno-unused-parameter)

    add_executable(rov_gateway src/main_gateway.cpp)
    target_link_libraries(rov_gateway PRIVATE gateway_core rov_control_lib)
else()
    add_executable(rov_gateway src/main.cpp)
    target_link_libraries(rov_gateway PRIVATE gateway_core)
endif()

# ---------------------------------------------------------------------------
# Host tests (Fake RovControl; the TCP test uses real loopback sockets).
enable_testing()

set(GW_TESTS
    crc16
    wire_codec
    frame_accumulator
    function_registry
    payload_codec
    gateway_state
    command_queue
    gateway_core
    tcp_server
    dht11_reader
    ina226_reader
    sensor_service
    ini_config
)

foreach(test_name IN LISTS GW_TESTS)
    add_executable(test_${test_name} tests/test_${test_name}.cpp)
    target_link_libraries(test_${test_name} PRIVATE gateway_core)
    target_include_directories(test_${test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME ${test_name} COMMAND test_${test_name})
endforeach()

# vendor/rov_control (copied unmodified from ROV_M33/A35/rov_control) is
# linked into the gateway from phase 4 onward; the phase 2/3 core talks to
# it through the IRovControl seam only.
