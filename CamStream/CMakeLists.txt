cmake_minimum_required(VERSION 3.10)
project(RGB_to_NV12_OpenCL CXX)

# 默认 Release(-O3): 对 memcpy/循环等热路径有明显收益
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall)

set(OPENCL_INCLUDE_DIRS "/usr/include")
set(OPENCL_LIBRARIES "/usr/lib/libOpenCL.so.1")

# GStreamer (appsrc 注入 + v4l2slh264enc 硬件编码推流)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

# 双模式: rgb_1080p (1080p@12fps) / rgb_720p (720p@30fps), 共用同一 OpenCL 内核
add_executable(rgb_1080p main_1080p.cpp)
add_executable(rgb_720p main_720p.cpp)

# 共享核心库: Phase 1 重构, 逐步从两个 main 中抽取公共模块
add_library(camstream_core STATIC
    core/dcmipp_setup.cpp
    core/v4l2_capture.cpp
    core/cl_converter.cpp
    core/gst_streamer.cpp
    core/status_printer.cpp
    core/isp_controller.cpp
    core/sensor_control.cpp
    core/camera_control.cpp
    core/control_console.cpp
    core/frame_dumper.cpp
    core/camera_config.cpp
    core/stream_app.cpp
)
target_include_directories(camstream_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(camstream_core PRIVATE ${OPENCL_INCLUDE_DIRS} ${GST_INCLUDE_DIRS})

# control_console 使用 std::thread
find_package(Threads REQUIRED)
target_link_libraries(camstream_core PUBLIC Threads::Threads)

foreach(target rgb_1080p rgb_720p)
    target_link_libraries(${target} camstream_core ${OPENCL_LIBRARIES} ${GST_LIBRARIES})
    target_include_directories(${target} PRIVATE ${OPENCL_INCLUDE_DIRS} ${GST_INCLUDE_DIRS})
endforeach()

# 将内核文件复制到构建目录, 以便程序运行时可以找到
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/rgb_to_nv12.cl
               ${CMAKE_CURRENT_BINARY_DIR}/rgb_to_nv12.cl COPYONLY)
