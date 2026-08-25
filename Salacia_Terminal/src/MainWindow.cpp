#include "MainWindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QLabel>
#include <QStatusBar>

#include "core/AppConfig.h"
#include "core/DataManager.h"
#include "core/Logger.h"
#include "recognition/OnnxInferEngine.h"
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
    aiStatsLabel_ = new QLabel(QString::fromLocal8Bit("AI：未启用"), this);
    statusBar()->addPermanentWidget(aiStatsLabel_);

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

    // ---- AI 推理接线（参数解耦：aiEnabled 来自 app_config.ini） ----
    if (AppConfig::instance().aiEnabled()) {
        aiEngine_ = std::make_unique<OnnxInferEngine>(this);

        connect(aiEngine_.get(), &OnnxInferEngine::backendReady, this,
                [this](const QString& backend) {
                    aiStatsLabel_->setText(QString::fromLocal8Bit("AI：%1").arg(backend));
                }, Qt::QueuedConnection);

        connect(aiEngine_.get(), &OnnxInferEngine::engineFailed, this,
                [this](const QString& reason) {
                    aiStatsLabel_->setText(QString::fromLocal8Bit("AI：不可用"));
                    statusBar()->showMessage(QString::fromLocal8Bit("AI 启动失败：") + reason, 10000);
                }, Qt::QueuedConnection);

        // 检测结果刷新 AI 状态（推理频率可达 30Hz，标签节流 200ms）
        connect(&DataManager::instance(), &DataManager::detectionsUpdated, this, [this] {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if ((now - lastAiLabelMs_) < 200) {
                return;
            }
            lastAiLabelMs_ = now;
            const std::size_t count = DataManager::instance().detections().size();
            aiStatsLabel_->setText(
                QString::fromLocal8Bit("AI：%1｜%2ms｜%3 目标")
                    .arg(aiEngine_->backendName())
                    .arg(aiEngine_->lastInferenceMs())
                    .arg(static_cast<uint>(count)));
        }, Qt::QueuedConnection);

        aiEngine_->start(&pipeline_->aiFrames());
    } else {
        aiStatsLabel_->setText(QString::fromLocal8Bit("AI：OFF（配置关闭）"));
    }
}

MainWindow::~MainWindow()
{
    // 独立于 closeEvent 的兜底：stop() 均幂等
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 逆序安全退出：先停网络接收（视频数据面），再停推理线程
    // （其内部在工作线程释放 ONNX/GPU 上下文），最后进入 GUI 析构
    pipeline_->stop();
    if (aiEngine_ != nullptr) {
        aiEngine_->stop();
    }
    Logger::info(QString::fromLocal8Bit("主窗口关闭：视频管线与推理引擎已停止"));
    event->accept();
}

} // namespace salacia
