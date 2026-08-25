#include "MainWindow.h"

#include <QCloseEvent>
#include <QLabel>
#include <QStatusBar>

#include "core/DataManager.h"
#include "core/Logger.h"
#include "video/GStreamerPipeline.h"
#include "widgets/VideoGLWidget.h"

namespace salacia {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QString::fromLocal8Bit("Salacia 水下机器人岸基终端"));
    resize(1280, 800);

    pipeline_ = std::make_unique<GStreamerPipeline>(this);

    videoWidget_ = new VideoGLWidget(this);
    videoWidget_->setSource(&pipeline_->displayFrames());
    setCentralWidget(videoWidget_);

    videoStatsLabel_ = new QLabel(QString::fromLocal8Bit("视频：等待流"), this);
    statusBar()->addPermanentWidget(videoStatsLabel_);

    // 管线错误 -> 状态栏（信号自主线程排队发出；显式 QueuedConnection 红线）
    connect(pipeline_.get(), &GStreamerPipeline::errorOccurred, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5000); },
            Qt::QueuedConnection);

    // 1Hz 链路统计 -> 状态栏标签（DataManager 信号，显式 QueuedConnection）
    connect(&DataManager::instance(), &DataManager::videoStatsUpdated, this, [this] {
        const VideoStats s = DataManager::instance().videoStats();
        const bool active = DataManager::instance().videoActive();
        videoStatsLabel_->setText(
            QString::fromLocal8Bit("视频：%1 fps｜丢帧 %2｜%3")
                .arg(s.fps, 0, 'f', 1)
                .arg(s.droppedFrames)
                .arg(active ? QString::fromLocal8Bit("在线")
                            : QString::fromLocal8Bit("离线")));
    }, Qt::QueuedConnection);

    if (!pipeline_->start()) {
        statusBar()->showMessage(QString::fromLocal8Bit("视频管线启动失败，详见日志"), 10000);
    }
}

MainWindow::~MainWindow()
{
    // 独立于 closeEvent 的兜底：管线 stop() 幂等
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 逆序安全退出：先停止视频数据面（生产者），再进入 GUI 析构
    pipeline_->stop();
    Logger::info(QString::fromLocal8Bit("主窗口关闭，视频管线已停止"));
    event->accept();
}

} // namespace salacia
