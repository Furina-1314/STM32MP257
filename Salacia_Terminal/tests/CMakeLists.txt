# 单元测试目标（QtTest；不混入主程序，无 GUI 依赖）
# 源码直接编入测试目标（保持主程序单一可执行结构；Phase 扩展时同步补源）

# 与主目标相同的字符集红线：Qt 默认注入 /utf-8 与 UNICODE 宏，
# 与全局 /source-charset:GBK 冲突（D8016/C4828），显式关闭；
# QtTest 宏与 cert/misra 检查不兼容，目录作用域关闭 clang-tidy
set(CMAKE_CXX_CLANG_TIDY "")

function(salacia_add_test_target name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(${name} PRIVATE
        Qt6::Core
        Qt6::Network   # AppConfig::resolveBindAddress 使用 QNetworkInterface
        Qt6::Test)
    set_target_properties(${name} PROPERTIES
        CLANG_TIDY ""
        QT_NO_UNICODE_DEFINES TRUE
        QT_NO_UTF8_SOURCE TRUE)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

salacia_add_test_target(salacia_tests_appconfig
    test_appconfig.cpp
    ../src/core/AppConfig.cpp
    ../src/core/Logger.cpp)

salacia_add_test_target(salacia_tests_wire
    test_wirecodec.cpp
    ../src/communication/WireCodec.cpp)

salacia_add_test_target(salacia_tests_registry
    test_registry.cpp
    ../src/communication/WireCodec.cpp
    ../src/communication/FunctionRegistry.cpp)

salacia_add_test_target(salacia_tests_tcp
    test_tcpclient.cpp
    mock_a35.h
    mock_a35.cpp
    ../src/communication/TcpClient.cpp
    ../src/communication/WireCodec.cpp
    ../src/communication/FunctionRegistry.cpp
    ../src/core/AppConfig.cpp
    ../src/core/Logger.cpp)

salacia_add_test_target(salacia_tests_sensor
    test_sensormodel.cpp
    ../src/sensor/SensorModel.cpp
    ../src/sensor/MPU6500Processor.cpp
    ../src/core/AppConfig.cpp
    ../src/core/Logger.cpp)

salacia_add_test_target(salacia_tests_alarm
    test_alarmmodel.cpp
    ../src/core/AlarmModel.cpp
    ../src/core/AppConfig.cpp
    ../src/core/Logger.cpp)

salacia_add_test_target(salacia_tests_safety
    test_safetystate.cpp
    ../src/core/SafetyStateModel.cpp
    ../src/communication/WireCodec.cpp
    ../src/communication/FunctionRegistry.cpp
    ../src/core/AppConfig.cpp
    ../src/core/Logger.cpp)

salacia_add_test_target(salacia_tests_phase6
    test_phase6.cpp
    mock_a35.h
    mock_a35.cpp
    ../src/communication/TcpClient.cpp
    ../src/communication/WireCodec.cpp
    ../src/communication/FunctionRegistry.cpp
    ../src/core/SafetyStateModel.cpp
    ../src/core/AppConfig.cpp
    ../src/core/Logger.cpp)

salacia_add_test_target(salacia_tests_controlvm
    test_controlvm.cpp
    ../src/control/ControlViewModel.cpp
    ../src/core/SafetyStateModel.cpp
    ../src/communication/WireCodec.cpp
    ../src/communication/FunctionRegistry.cpp
    ../src/core/AppConfig.cpp
    ../src/core/Logger.cpp)

salacia_add_test_target(salacia_tests_videohub
    test_videohub.cpp
    ../src/video/VideoFrameHub.h)
