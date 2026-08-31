#include "MainWindow.h"

// 崩溃迷你转储（野外终端可诊断性设施）：未处理异常时在 logs/ 生成
// salacia_crash_<时间>.dmp，便于 WinDbg/cdb 定位崩溃栈
#include <windows.h>
#include <dbghelp.h>

namespace {
LONG WINAPI writeMiniDump(EXCEPTION_POINTERS* info)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    const int n = wsprintfW(path, L"logs\\salacia_crash_%04u%02u%02u_%02u%02u%02u.dmp",
                            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (n > 0) {
        const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = info;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                              MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QPalette>
#include <QtWidgets/QApplication>

#include "fluentui3style.h"
#include "fluentuiappearance.h"

#include "core/AppConfig.h"
#include "core/Logger.h"

int main(int argc, char *argv[])
{
    SetUnhandledExceptionFilter(&writeMiniDump);
    // 红线：任何 GL 渲染路径（QQuickWidget / GStreamer OpenGL 对接）必须
    // 在 QApplication 构造之前强制 OpenGL RHI，规避 Qt6 默认 D3D11 后端
    // 与 GStreamer OpenGL 插件的底层上下文冲突
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Salacia_Terminal"));
    QCoreApplication::setOrganizationName(QStringLiteral("Salacia"));

    // 1) 配置（参数解耦红线：全部可调参数来自 config/app_config.ini）
    salacia::AppConfig& cfg = salacia::AppConfig::instance();
    cfg.load();

    // 1b) FluentUIStyle 界面样式（[ui] style/theme/palette；浅色+Fluent 默认）
    {
        const QString& styleName = cfg.uiStyleName();
        if (styleName.compare(QStringLiteral("FluentUI3"), Qt::CaseInsensitive) == 0) {
            app.setStyle(new FluentUI3Style());
            fluentUIAppearance.initialize();
            fluentUIAppearance.setTheme(cfg.uiTheme() == QStringLiteral("dark")
                                                 ? Theme::Dark
                                                 : Theme::Light);
            // 配色：0 = Fluent，1 = Teams
            app.setProperty("_q_themestyle",
                            cfg.uiPalette() == QStringLiteral("teams") ? 1 : 0);
            // 自定义强调色（空 = 库默认 Fluent 蓝；设置页即时切换并写回）
            const QString accent = cfg.uiAccentColor();
            if (!accent.isEmpty()) {
                QPalette pal = app.palette();
                const QColor color(accent);
                pal.setColor(QPalette::Accent, color);
                pal.setColor(QPalette::Highlight, color);
                app.setPalette(pal);
            }
        }
    }

    // 2) 日志（后台日志线程；目录/级别来自配置；摘要在其后输出）
    salacia::Logger::init(cfg.logDir());
    cfg.logSummary();

    // 3) 主窗口：GUI 线程只承担事件循环与渲染
    int exitCode = 0;
    {
        salacia::MainWindow window;
        window.show();
        exitCode = app.exec();
    } // 窗口先于日志系统析构（析构期间仍可安全写日志）

    // 4) 逆序安全退出：主窗口已销毁 -> 停止日志线程 -> 关闭日志文件
    salacia::Logger::shutdown();

    // 已知问题规避（TD-8）：Intel Iris Xe OpenGL ICD（igxelpicd64.dll）
    // 在带流拆卸后的进程退出阶段确定性崩溃（+0xCF6CDB，空函数指针调用，
    // 已取崩溃转储证实）。全部自有资源已在上方逆序停止且日志已落盘，
    // 此处直接 ExitProcess 跳过驱动卸载路径（内核回收全部资源）。
    // 静默期：让驱动/GStreamer 残留工作线程排空，避免强杀中途故障
    //（时长来自 [system] exit_grace_ms）
    ::Sleep(static_cast<DWORD>(cfg.exitGraceMs()));
    ::ExitProcess(static_cast<UINT>(exitCode));
}
