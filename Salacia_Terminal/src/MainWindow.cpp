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
        // 红线：Worker-Object 禁止设置父对象，否则 moveToThread 失败、
        // 对象滞留 GUI 线程（曾导致退出死锁）；生命周期由 unique_ptr 管理
        aiEngine_ = std::make_unique<OnnxInferEngine>();

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
    // 逆序安全退出：先停视频绘制并排空 GPU（消除 GL/d3d11 驱动层竞态），
    // 再停网络接收（视频数据面），再停推理线程（其内部在工作线程释放
    // ONNX/GPU 上下文），最后进入 GUI 析构
    videoWidget_->releaseGl();
    pipeline_->stopForExit();
    pipeline_.release(); // 管线已停流并故意泄漏（TD-8），放弃所有权
    if (aiEngine_ != nullptr) {
        aiEngine_->stop();
    }
    Logger::info(QString::fromLocal8Bit("主窗口关闭：视频管线与推理引擎已停止"));

    // TD-8 规避：退出阶段销毁 QOpenGLWidget 会触发 Intel Iris Xe ICD
    // 确定性崩溃（igxelpicd64.dll 空函数指针调用，转储证实）。
    // 资源已在上方全部停止，此处将 GL 部件脱离父子链故意泄漏，
    // 跳过其析构，进程退出由内核统一回收。
    videoWidget_->setParent(nullptr);
    videoWidget_ = nullptr;

    event->accept();
}

} // namespace salacia
