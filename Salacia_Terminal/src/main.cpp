#include "MainWindow.h"

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtWidgets/QApplication>

#include "core/AppConfig.h"
#include "core/Logger.h"

int main(int argc, char *argv[])
{
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
    return exitCode;
}
