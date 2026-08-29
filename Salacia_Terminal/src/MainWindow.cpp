#include "MainWindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDockWidget>
#include <QFormLayout>
#include <QLabel>
#include <QQmlContext>
#include <QQuickWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include "communication/SshClient.h"
#include "communication/UdpReceiver.h"
#include "core/AppConfig.h"
#include "core/DataManager.h"
#include "core/Logger.h"
#include "recognition/OnnxInferEngine.h"
#include "sensor/RovVizModel.h"
#include "video/GStreamerPipeline.h"
#include "widgets/ControlPanelWidget.h"
#include "widgets/VideoGLWidget.h"

namespace salacia {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QString::fromLocal8Bit("Salacia Terminal"));
    resize(1440, 860);

    pipeline_ = std::make_unique<GStreamerPipeline>(this);
    telemetryReceiver_ = std::make_unique<UdpReceiver>(); // 无父：Worker 红线

    // ---- 中央视频区 ----
    videoWidget_ = new VideoGLWidget(this);
    videoWidget_->setSource(&pipeline_->displayFrames());
    setCentralWidget(videoWidget_);

    // ---- 左侧执行机构遥控坞（16 路 PWM：10 舵机 + 6 推进器） ----
    sshClient_ = std::make_unique<SshClient>(); // 无父：Worker 红线
    controlPanel_ = new ControlPanelWidget(this);
    connect(controlPanel_, &ControlPanelWidget::pwmCommandRequested, this,
            [this](int deviceId, int pulseUs) {
                // 板端 CLI 约定：pwm <id> <us>（id 1-10 舵机 / 11-16 推进器）
                sshClient_->requestCommand(
                        QString::fromLatin1("pwm %1 %2").arg(deviceId).arg(pulseUs));
            });
    connect(controlPanel_, &ControlPanelWidget::emergencyStopRequested, this, [this] {
                statusBar()->showMessage(
                        QString::fromLocal8Bit("紧急停机已下发：推进器中位/舵机回中"), 3000);
            });
    QDockWidget* controlDock = new QDockWidget(QString::fromLocal8Bit("执行机构遥控"), this);
    controlDock->setWidget(controlPanel_);
    controlDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::LeftDockWidgetArea, controlDock);

    // ---- 右侧舱体状态坞 ----
    rovViz_ = new RovVizModel(this);
    rovViz_->bindToDataManager();
    QDockWidget* dock = new QDockWidget(QString::fromLocal8Bit("舱体状态"), this);
    dock->setWidget(createStatusDock());
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    // ---- 状态栏 ----
    videoStatsLabel_ = new QLabel(QString::fromLocal8Bit("视频：等待流"), this);
    statusBar()->addPermanentWidget(videoStatsLabel_);
    aiStatsLabel_ = new QLabel(QString::fromLocal8Bit("AI：未启用"), this);
    statusBar()->addPermanentWidget(aiStatsLabel_);
    telemetryLabel_ = new QLabel(QString::fromLocal8Bit("遥测：等待"), this);
    statusBar()->addPermanentWidget(telemetryLabel_);
    sshLabel_ = new QLabel(QString::fromLocal8Bit("SSH：连接中"), this);
    statusBar()->addPermanentWidget(sshLabel_);

    // 管线错误 -> 状态栏（显式 QueuedConnection 红线）
    connect(pipeline_.get(), &GStreamerPipeline::errorOccurred, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5000); },
            Qt::QueuedConnection);

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

    connect(telemetryReceiver_.get(), &UdpReceiver::telemetryActiveChanged, this,
            [this](bool active) {
                telemetryLabel_->setText(active
                        ? QString::fromLocal8Bit("遥测：在线")
                        : QString::fromLocal8Bit("遥测：离线"));
            }, Qt::QueuedConnection);
    connect(telemetryReceiver_.get(), &UdpReceiver::receiverError, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5000); },
            Qt::QueuedConnection);

    // ---- 启动各数据面（配置门控） ----
    if (!pipeline_->start()) {
        statusBar()->showMessage(QString::fromLocal8Bit("视频管线启动失败，详见日志"), 10000);
    }
    telemetryReceiver_->start();

    // SSH 遥控通道（参数全部来自 ini [rov]）
    {
        SshClient::Settings sshSettings;
        const AppConfig& cfg = AppConfig::instance();
        sshSettings.host = cfg.sshHost();
        sshSettings.port = cfg.sshPort();
        sshSettings.user = cfg.sshUser();
        sshSettings.password = cfg.sshPassword();
        sshSettings.keyPath = cfg.sshKeyPath();
        sshSettings.reconnectSec = cfg.sshReconnectSec();
        connect(sshClient_.get(), &SshClient::connectionStateChanged, this,
                [this](bool on) {
                    sshLabel_->setText(on ? QString::fromLocal8Bit("SSH：在线")
                                          : QString::fromLocal8Bit("SSH：离线"));
                    controlPanel_->setLinkStatus(on);
                }, Qt::QueuedConnection);
        connect(sshClient_.get(), &SshClient::clientError, this,
                [this](const QString& message) {
                    Logger::warning(message);
                }, Qt::QueuedConnection);
        sshClient_->start(sshSettings);
    }

    if (AppConfig::instance().aiEnabled()) {
        // Worker 一律无父创建（moveToThread 红线）；生命周期由 unique_ptr 管理
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
    // 兜底：stop() 均幂等
}

QWidget* MainWindow::createStatusDock()
{
    QWidget* panel = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(panel);

    // 三维姿态（Quick3D，OpenGL RHI 由 main.cpp 全局强制）
    QQuickWidget* quick = new QQuickWidget(panel);
    quick->setClearColor(QColor(0x14, 0x19, 0x22));
    quick->rootContext()->setContextProperty(QStringLiteral("rovViz"), rovViz_);
    quick->setSource(QUrl(QStringLiteral("qrc:/qml/RovViz.qml")));
    quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick->setMinimumHeight(340);
    layout->addWidget(quick, 1);

    // 传感器表单（5Hz 节流刷新）
    QFormLayout* form = new QFormLayout();
    rollLabel_ = new QLabel(QString::fromLocal8Bit("--"), panel);
    pitchLabel_ = new QLabel(QString::fromLocal8Bit("--"), panel);
    yawLabel_ = new QLabel(QString::fromLocal8Bit("--"), panel);
    tempLabel_ = new QLabel(QString::fromLocal8Bit("--"), panel);
    humidLabel_ = new QLabel(QString::fromLocal8Bit("--"), panel);
    batteryLabel_ = new QLabel(QString::fromLocal8Bit("--"), panel);
    form->addRow(QString::fromLocal8Bit("横滚 Roll"), rollLabel_);
    form->addRow(QString::fromLocal8Bit("俯仰 Pitch"), pitchLabel_);
    form->addRow(QString::fromLocal8Bit("航向 Yaw"), yawLabel_);
    form->addRow(QString::fromLocal8Bit("舱内温度"), tempLabel_);
    form->addRow(QString::fromLocal8Bit("舱内湿度"), humidLabel_);
    form->addRow(QString::fromLocal8Bit("电池电量"), batteryLabel_);
    layout->addLayout(form);

    connect(rovViz_, &RovVizModel::stateChanged, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if ((now - lastPanelMs_) < 200) {
            return; // 20Hz 数据 -> 5Hz 表单
        }
        lastPanelMs_ = now;
        rollLabel_->setText(QString::fromLocal8Bit("%1 °").arg(rovViz_->rollDeg(), 0, 'f', 1));
        pitchLabel_->setText(QString::fromLocal8Bit("%1 °").arg(rovViz_->pitchDeg(), 0, 'f', 1));
        yawLabel_->setText(QString::fromLocal8Bit("%1 °").arg(rovViz_->yawDeg(), 0, 'f', 1));
        tempLabel_->setText(QString::fromLocal8Bit("%1 °C").arg(rovViz_->cabinTempC(), 0, 'f', 1));
        humidLabel_->setText(QString::fromLatin1("%1 %RH").arg(rovViz_->cabinHumidityPct(), 0, 'f', 1));
        batteryLabel_->setText(QString::fromLocal8Bit("%1 %（%2 V）")
                                   .arg(rovViz_->batteryPercent(), 0, 'f', 0)
                                   .arg(rovViz_->batteryVoltage(), 0, 'f', 2));
    }, Qt::QueuedConnection);

    return panel;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 逆序安全退出：先停视频绘制并排空 GPU（消除 GL/d3d11 驱动层竞态），
    // 再停网络接收（视频数据面 + 遥测），再停推理线程（其内部在工作线程
    // 释放 ONNX/GPU 上下文），最后进入 GUI 析构
    videoWidget_->releaseGl();
    pipeline_->stopForExit();
    pipeline_.release(); // 管线已停流并故意泄漏（TD-8），放弃所有权
    telemetryReceiver_->stop();
    sshClient_->stop();
    if (aiEngine_ != nullptr) {
        aiEngine_->stop();
    }
    Logger::info(QString::fromLocal8Bit("主窗口关闭：视频/遥测/SSH/推理已全部停止"));

    // TD-8 规避：退出阶段销毁 QOpenGLWidget 会触发 Intel Iris Xe ICD
    // 确定性崩溃（igxelpicd64.dll 空函数指针调用，转储证实）。
    // 资源已在上方全部停止，此处将 GL 部件脱离父子链故意泄漏，
    // 跳过其析构，进程退出由内核统一回收。
    videoWidget_->setParent(nullptr);
    videoWidget_ = nullptr;

    event->accept();
}

} // namespace salacia
