# FluentUIStyle 界面库（复制自 FluentUIStyle-3.0，MIT；qwindowkit Apache-2.0）
# 见 LICENSE.FluentUIStyle.txt / THIRD_PARTY_NOTICES.md
#
# 源码为 UTF-8（中文注释）——本目录目标统一 /source-charset:utf-8 覆盖全局 GBK；
# 第三方代码关闭 clang-tidy 与警告上抛。

# ---- QWindowKit（静态；仅 Widgets 模块）----
# 第三方目录作用域关闭 clang-tidy（继承自顶层，曾致 390KB 样式源 OOM）
set(CMAKE_CXX_CLANG_TIDY "")
set(QWINDOWKIT_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(QWINDOWKIT_BUILD_WIDGETS ON CACHE BOOL "" FORCE)
set(QWINDOWKIT_BUILD_QUICK OFF CACHE BOOL "" FORCE)
set(QWINDOWKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QWINDOWKIT_BUILD_DOCUMENTATIONS OFF CACHE BOOL "" FORCE)
set(QWINDOWKIT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(3rd/qwindowkit EXCLUDE_FROM_ALL)

# ---- FluentUI3Style（静态）----
add_library(salacia_fluentstyle STATIC
    fluentui3style/fluentui3style.cpp
    fluentui3style/fluentui3style.h
    fluentui3style/fluentui3style_global.h
    fluentui3style/fluentui3styleproperties.h
    fluentui3style/fluentuiappearance.cpp
    fluentui3style/fluentuiappearance.h
    fluentui3style/palettemanager.cpp
    fluentui3style/palettemanager.h
    fluentui3style/qhexstring_p.h
    fluentui3style/qstyleanimation.cpp
    fluentui3style/qstyleanimation_p.h
    fluentui3style/qstylehelper.cpp
    fluentui3style/qstylehelper_p.h
    fluentui3style/comboboxpopupanimation_p.cpp
    fluentui3style/comboboxpopupanimation_p.h
    fluentui3style/menupopupanimation_p.cpp
    fluentui3style/menupopupanimation_p.h
    fluentui3style/resource.qrc
)
add_library(Salacia::FluentStyle ALIAS salacia_fluentstyle)

target_compile_definitions(salacia_fluentstyle PRIVATE
    FLUENT_USE_QT_STYLE FLUENTUI3STYLE_LIBRARY)

target_include_directories(salacia_fluentstyle PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/fluentui3style
    ${CMAKE_CURRENT_SOURCE_DIR}/FluentUI3Colors)

target_link_libraries(salacia_fluentstyle PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
    Qt6::CorePrivate Qt6::GuiPrivate Qt6::WidgetsPrivate)

if(WIN32)
    target_link_libraries(salacia_fluentstyle PRIVATE dwmapi)
endif()

# ---- ExWidgets 子集（静态）----
add_library(salacia_exwidgets STATIC
    exwidgets/exwidgetsmacros.h
    exwidgets/exwidgets_global.h
    exwidgets/exwinuinavigationview.cpp
    exwidgets/exwinuinavigationview.h
    exwidgets/exnavtreewidget.cpp
    exwidgets/exnavtreewidget.h
    exwidgets/exstackedwidget.cpp
    exwidgets/exstackedwidget.h
    exwidgets/excontentdialog.cpp
    exwidgets/excontentdialog.h
    exwidgets/exmessagebox.cpp
    exwidgets/exmessagebox.h
    exwidgets/exinfobar.cpp
    exwidgets/exinfobar.h
    exwidgets/exinfobarhost.cpp
    exwidgets/exinfobarhost.h
    exwidgets/exexpander.cpp
    exwidgets/exexpander.h
    exwidgets/extabwidget.cpp
    exwidgets/extabwidget.h
    exwidgets/exprogressring.cpp
    exwidgets/exprogressring.h
)
add_library(Salacia::ExWidgets ALIAS salacia_exwidgets)

target_compile_definitions(salacia_exwidgets PRIVATE EXWIDGETS_LIBRARY)

target_include_directories(salacia_exwidgets PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/exwidgets
    ${CMAKE_CURRENT_SOURCE_DIR}/fluentui3style)

target_link_libraries(salacia_exwidgets PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets
    Salacia::FluentStyle)

# ---- Frameless（FluentTitleBar / FluentWindowFrame，静态）----
add_library(salacia_frameless STATIC
    exwidgets/frameless/exwidgetsframeless_global.h
    exwidgets/frameless/fluenttitlebar.cpp
    exwidgets/frameless/fluenttitlebar.h
    exwidgets/frameless/fluentwindowframe.cpp
    exwidgets/frameless/fluentwindowframe.h
)
add_library(Salacia::Frameless ALIAS salacia_frameless)

target_compile_definitions(salacia_frameless PRIVATE EXWIDGETS_FRAMELESS_LIBRARY)

target_include_directories(salacia_frameless PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/exwidgets/frameless)

target_link_libraries(salacia_frameless PUBLIC
    Salacia::ExWidgets
    Qt6::Core Qt6::Gui Qt6::Widgets
    QWindowKit::Widgets)

# ---- 本目录第三方目标：UTF-8 源、关 tidy、不作为新增告警源 ----
set(_salacia_ui_targets salacia_fluentstyle salacia_exwidgets salacia_frameless)
foreach(_tgt ${_salacia_ui_targets})
    set_target_properties(${_tgt} PROPERTIES
        CLANG_TIDY ""
        UNITY_BUILD OFF
        AUTORCC ON
        # 阻止 Qt 注入 /utf-8（与下方显式 /source-charset:utf-8 冲突，D8016）
        QT_NO_UTF8_SOURCE TRUE)
    if(MSVC)
        # 源码已统一转为 GBK（与主工程同字符集；中文字面量经 fromLocal8Bit）
        target_compile_options(${_tgt} PRIVATE /wd4273 /wd4251)
    endif()
endforeach()
