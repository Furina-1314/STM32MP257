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

add_executable(rgb_to_nv12 main.cpp)

target_link_libraries(rgb_to_nv12 ${OPENCL_LIBRARIES} ${GST_LIBRARIES})
target_include_directories(rgb_to_nv12 PRIVATE ${OPENCL_INCLUDE_DIRS} ${GST_INCLUDE_DIRS})

# 将内核文件复制到构建目录, 以便程序运行时可以找到
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/rgb_to_nv12.cl
               ${CMAKE_CURRENT_BINARY_DIR}/rgb_to_nv12.cl COPYONLY)
