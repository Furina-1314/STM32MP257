cmake_minimum_required(VERSION 3.20)

# =============================================================================
#  Salacia_Terminal —— 水下机器人（ROV）岸上控制终端
#
#  环境红线（禁止未经声明的替代方案）：
#    - Windows 11 / Visual Studio 2026（MSVC）/ CMake >= 3.20 / Ninja
#    - GBK 多字节字符集（禁用 Unicode 字符集；源码以 GBK 保存，
#      与 QString 交互一律 QString::fromLocal8Bit()）
#    - Qt 6.11.1 / GStreamer 1.20+（RTP-H264 低延迟管线）/ ONNX Runtime C++ API
#    - C++17；MISRA/CERT 静态分析（Clang-Tidy）；禁裸指针（除第三方 C 库边界）
# =============================================================================

project(Salacia_Terminal VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 为 clang-tidy 与 IDE 导出编译数据库
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# 1. 静态代码分析：Clang-Tidy（MISRA / CERT）
# -----------------------------------------------------------------------------
find_program(CLANG_TIDY_EXE
    NAMES clang-tidy
    HINTS
        "$ENV{VS_LLVM_DIR}"
        "F:/Microsoft Visual Studio/18/Insiders/VC/Tools/Llvm/x64/bin"
        "C:/Program Files/LLVM/bin"
)
if(CLANG_TIDY_EXE)
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-checks=-*,cert-*,misra-*")
    message(STATUS "Clang-Tidy   : ${CLANG_TIDY_EXE}")
else()
    message(WARNING "clang-tidy 未找到，MISRA/CERT 静态分析跳过（安装 LLVM 或 VS 的 Clang 工具集后重新配置即可启用）")
endif()

# -----------------------------------------------------------------------------
# 2. Qt 6.11.1
# -----------------------------------------------------------------------------
include(qt.cmake) # 兼容宏（Qt >= 6.3 时无实际作用）

find_package(Qt6 6.11.1 REQUIRED COMPONENTS
    Core
    Gui
    Widgets        # 主界面框架（Widgets 红线）
    Quick          # QML：视频组件与 Quick3D 载体
    Quick3D        # 3D 姿态实时显示
    Network        # UDP 传感器接收
    Concurrent     # 推理 / 通信后台任务
)
qt_standard_project_setup()

# -----------------------------------------------------------------------------
# 3. GBK 多字节字符集（核心红线）
#    Qt6 在 Windows 下默认注入 UNICODE/_UNICODE 宏与 -utf-8 编译选项；
#    通过 Qt6 官方目标属性 QT_NO_UNICODE_DEFINES / QT_NO_UTF8_SOURCE 关闭
#    （在可执行目标上设置，见第 6 节），并显式声明 _MBCS + GBK 字符集。
# -----------------------------------------------------------------------------
if(WIN32)
    add_compile_definitions(_MBCS) # 对应 VS 项目「多字节字符集」属性
    if(MSVC)
        add_compile_options(/source-charset:GBK /execution-charset:GBK)
    endif()
endif()

# -----------------------------------------------------------------------------
# 4. GStreamer 1.20+（RTP/H264 接收、硬件解码、tee GPU 分流）
#    官方 MSVC 包未提供 CMake config，手动定位头文件与导入库。
#    注意：glibconfig.h 位于 lib/glib-2.0/include；功能库为短名 gstXXX-1.0.lib。
# -----------------------------------------------------------------------------
set(GSTREAMER_ROOT "F:/gstreamer/1.0/msvc_x86_64" CACHE PATH
    "GStreamer MSVC x86_64 安装根目录")

find_path(GST_GSTREAMER_INCLUDE_DIR gst/gst.h
    HINTS "${GSTREAMER_ROOT}/include/gstreamer-1.0" REQUIRED)
find_path(GST_GLIB_INCLUDE_DIR glib.h
    HINTS "${GSTREAMER_ROOT}/include/glib-2.0" REQUIRED)
find_path(GST_GLIBCONFIG_INCLUDE_DIR glibconfig.h
    HINTS "${GSTREAMER_ROOT}/lib/glib-2.0/include" REQUIRED)

find_library(GST_GSTREAMER_LIBRARY gstreamer-1.0 HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)
find_library(GST_GOBJECT_LIBRARY    gobject-2.0    HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)
find_library(GST_GLIB_LIBRARY       glib-2.0       HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)
find_library(GST_BASE_LIBRARY       gstbase-1.0    HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)
find_library(GST_APP_LIBRARY        gstapp-1.0     HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)
find_library(GST_VIDEO_LIBRARY      gstvideo-1.0   HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)
find_library(GST_GL_LIBRARY         gstgl-1.0      HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)
find_library(GST_RTP_LIBRARY        gstrtp-1.0     HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)
find_library(GST_PBUTILS_LIBRARY    gstpbutils-1.0 HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)

add_library(Gst::GStreamer INTERFACE IMPORTED)
set_target_properties(Gst::GStreamer PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES
        "${GST_GSTREAMER_INCLUDE_DIR};${GST_GLIB_INCLUDE_DIR};${GST_GLIBCONFIG_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES
        "${GST_GSTREAMER_LIBRARY};${GST_GOBJECT_LIBRARY};${GST_GLIB_LIBRARY}")

# 功能库目标：仅依赖核心目标，按 Phase 逐步接入链接
function(salacia_add_gst_lib _target _library)
    add_library(${_target} INTERFACE IMPORTED)
    set_target_properties(${_target} PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_library};Gst::GStreamer")
endfunction()

salacia_add_gst_lib(Gst::Base    "${GST_BASE_LIBRARY}")
salacia_add_gst_lib(Gst::App     "${GST_APP_LIBRARY}")
salacia_add_gst_lib(Gst::Video   "${GST_VIDEO_LIBRARY}")
salacia_add_gst_lib(Gst::GL      "${GST_GL_LIBRARY}")
salacia_add_gst_lib(Gst::Rtp     "${GST_RTP_LIBRARY}")
salacia_add_gst_lib(Gst::PbUtils "${GST_PBUTILS_LIBRARY}")

message(STATUS "GStreamer    : ${GSTREAMER_ROOT}")

# -----------------------------------------------------------------------------
# 5. ONNX Runtime 1.24.4（AI 推理，Phase 4 接入）
#    多平台 EP 策略，三套 C++ 发行版已安装于 F:/onnxruntime（同版本 1.24.4，
#    统一 C++ API，避免变体间接口漂移）：
#      - directml-1.24.4 —— DirectML EP，本机 Intel Iris Xe 核显（默认）
#      - gpu-1.24.4     —— CUDA/TensorRT EP，NVIDIA RTX 独显平台
#                          （运行期另需 CUDA Toolkit + cuDNN）
#      - cpu-1.24.4     —— CPU EP，兜底与调试
#    切换变体：cmake -DONNXRUNTIME_ROOT=F:/onnxruntime/<变体目录>
#    （Python 侧 pip 1.29.0 仅用于 .pt -> .onnx 转换，与 C++ 版本无关）
# -----------------------------------------------------------------------------
set(ONNXRUNTIME_ROOT "F:/onnxruntime/directml-1.24.4" CACHE PATH
    "ONNX Runtime C++ 根目录（标准布局：include/ + lib/ + bin/）")

find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS "${ONNXRUNTIME_ROOT}/include" "${ONNXRUNTIME_ROOT}")
find_library(ONNXRUNTIME_LIBRARY onnxruntime
    HINTS "${ONNXRUNTIME_ROOT}/lib" "${ONNXRUNTIME_ROOT}/capi")

if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    add_library(Ort::OnnxRuntime INTERFACE IMPORTED)
    set_target_properties(Ort::OnnxRuntime PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${ONNXRUNTIME_LIBRARY}")
    message(STATUS "ONNX Runtime : ${ONNXRUNTIME_LIBRARY}")
else()
    message(WARNING
        "ONNX Runtime C++ 开发文件未就绪：ONNXRUNTIME_ROOT 为 pip 目录，"
        "仅含 onnxruntime.dll（1.29.0，CPU/Azure EP），缺少 onnxruntime_cxx_api.h "
        "与 onnxruntime.lib。请下载官方 onnxruntime-win-x64[-gpu]-1.29.0 发行包解压后，"
        "将 ONNXRUNTIME_ROOT 指向该目录。当前不阻断配置，Phase 4 前必须解决。")
endif()

# -----------------------------------------------------------------------------
# 6. 可执行目标（Phase 1：Qt Widgets 骨架）
# -----------------------------------------------------------------------------
set(PROJECT_SOURCES
    src/main.cpp
    src/Salacia_Terminal.h
    src/Salacia_Terminal.cpp
    src/Salacia_Terminal.ui
    src/core/Logger.h
    src/core/Logger.cpp
)

qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

set_target_properties(${PROJECT_NAME} PROPERTIES
    WIN32_EXECUTABLE TRUE
    QT_NO_UNICODE_DEFINES TRUE # Qt6：不注入 UNICODE/_UNICODE 宏（GBK 红线）
    QT_NO_UTF8_SOURCE TRUE     # Qt6：不注入 -utf-8（与 /source-charset:GBK 冲突）
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    # Phase 3 接入：Gst::GStreamer Gst::App Gst::Video Gst::GL Gst::Rtp
    # Phase 4 接入：Ort::OnnxRuntime
    # Phase 5 接入：Qt6::Quick Qt6::Quick3D Qt6::Network
    # Phase 6 接入：Qt6::Concurrent + libssh
)
