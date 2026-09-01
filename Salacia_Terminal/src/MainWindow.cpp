#include "MainWindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QQmlContext>
#include <QQuickWidget>
#include <QPushButton>
#include <QSplitter>
#include <QToolButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include "communication/FunctionRegistry.h"
#include "communication/TcpClient.h"
#include "communication/UdpReceiver.h"
#include "communication/WireConstants.h"
#include "control/ControlViewModel.h"
#include "core/AlarmModel.h"
#include "core/AppConfig.h"
#include "core/DataManager.h"
#include "core/Logger.h"
#include "core/SafetyStateModel.h"
#include "exmessagebox.h"
#include "exinfobarhost.h"
#include "exwinuinavigationview.h"
#include "fluenttitlebar.h"
#include "fluentwindowframe.h"
#include "recognition/OnnxInferEngine.h"
#include "sensor/RovVizModel.h"
#include "sensor/SensorModel.h"
#include "video/GStreamerPipeline.h"
#include "widgets/AboutPageWidget.h"
#include "widgets/AlarmBarWidget.h"
#include "widgets/CommandPageWidget.h"
#include "widgets/ControlAreaWidget.h"
#include "widgets/SettingsPageWidget.h"
#include "widgets/VideoGLWidget.h"

namespace salacia {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    const AppConfig& cfg = AppConfig::instance();

    setWindowTitle(QString::fromLocal8Bit("Salacia Terminal"));
    resize(cfg.windowWidth(), cfg.windowHeight());

    // ---- Fluent frameless 窗口 chrome ----
    auto* windowFrame = new FluentWindowFrame(this, this);
    windowFrame->installChromeHeader(nullptr);
    titleBar_ = windowFrame->titleBar();
    if (titleBar_ != nullptr) {
        titleBar_->setThemeDark(cfg.uiTheme() == QStringLiteral("dark"));
        // 用户要求：移除搜索框、浅/深色按钮、置顶按钮（仅保留系统三键与标题）
        titleBar_->searchLineEdit()->hide();
        titleBar_->themeButton()->hide();
        titleBar_->pinButton()->hide();
    }

    // 告警弹窗宿主（窗口级 ExInfoBar，默认 4.5s 超时）
    ExInfoBarHost::setDefaultTarget(this);

    // ---- 模型层 ----
    alarmModel_ = std::make_unique<AlarmModel>(this);
    safety_ = std::make_unique<SafetyStateModel>(this);
    controlVm_ = std::make_unique<ControlViewModel>(safety_.get(), this);

    pipeline_ = std::make_unique<GStreamerPipeline>(this);
    telemetryReceiver_ = std::make_unique<UdpReceiver>();
    rovViz_ = new RovVizModel(this);
    rovViz_->bindToDataManager();

    // ---- 中央区：左导航 + 右内容 ----
    auto* central = new QWidget(this);
    auto* centralLayout = new QHBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    auto* navColumn = new QWidget(central);
    auto* navLayout = new QVBoxLayout(navColumn);
    navLayout->setContentsMargins(0, 4, 0, 0);
    navLayout->setSpacing(0);
    navToggleBtn_ = new QPushButton(QString::fromLocal8Bit("收起菜单"), navColumn);
    navToggleBtn_->setFlat(true);
    navLayout->addWidget(navToggleBtn_);
    nav_ = new ExWinUINavigationView(navColumn);
    nav_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
    navLayout->addWidget(nav_);
    centralLayout->addWidget(navColumn);

    auto* content = new QWidget(central);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(4, 4, 4, 4);

    alarmBar_ = new AlarmBarWidget(alarmModel_.get(), content);
    contentLayout->addWidget(alarmBar_);

    stack_ = new QStackedWidget(content);
    stack_->addWidget(createHomePage());          // 0 主页
    commandPage_ = new CommandPageWidget(controlVm_.get(), safety_.get(), stack_);
    stack_->addWidget(commandPage_);               // 1 指令
    settingsPage_ = new SettingsPageWidget(
            const_cast<AppConfig&>(AppConfig::instance()), stack_); // 2 设置
    stack_->addWidget(settingsPage_);
    aboutPage_ = new AboutPageWidget(stack_);      // 3 关于
    stack_->addWidget(aboutPage_);
    contentLayout->addWidget(stack_, 1);

    centralLayout->addWidget(content, 1);
    setCentralWidget(central);

    // 导航图标：QChar 码点构造（GBK 源码中 \uXXXX 转义破坏窄字符串曾致乱码）
    nav_->addMainNavigationItem(QString::fromLocal8Bit("主页"), 0,
                                QString(QChar(0xE80F)));
    nav_->addMainNavigationItem(QString::fromLocal8Bit("指令"), 1,
                                QString(QChar(0xE756)));
    nav_->addFooterNavigationItem(QString::fromLocal8Bit("设置"), 2,
                                  QString(QChar(0xE713)));
    nav_->addFooterNavigationItem(QString::fromLocal8Bit("关于"), 3,
                                  QString(QChar(0xE946)));
    nav_->setStackedWidget(stack_);
    nav_->setNavigationExpanded(true, false);
    nav_->setSelectedPageIndex(0);

    connect(navToggleBtn_, &QPushButton::clicked, this, [this] {
        // 保持窗口尺寸不变：记录当前几何，切换后恢复（含最大化/全屏状态不破坏）
        const bool wasMax = isMaximized();
        const QRect savedGeo = normalGeometry();
        const bool expanded = nav_->navigationExpanded();
        nav_->setNavigationExpanded(!expanded);
        navToggleBtn_->setText(expanded ? QString::fromLocal8Bit("展开菜单")
                                        : QString::fromLocal8Bit("收起菜单"));
        if (!wasMax) {
            setGeometry(savedGeo); // 非最大化：恢复原窗口矩形
        }
        // 最大化时 setGeometry 会破坏全屏状态——不调即可（Qt 布局在客户区内自适应）
    });

    // ---- 状态栏 ----
    videoStatsLabel_ = new QLabel(QString::fromLocal8Bit("视频：等待流"), this);
    statusBar()->addPermanentWidget(videoStatsLabel_);
    aiStatsLabel_ = new QLabel(QString::fromLocal8Bit("AI：未启用"), this);
    statusBar()->addPermanentWidget(aiStatsLabel_);
    telemetryLabel_ = new QLabel(QString::fromLocal8Bit("遥测：等待"), this);
    statusBar()->addPermanentWidget(telemetryLabel_);
    tcpLabel_ = new QLabel(QString::fromLocal8Bit("TCP：连接中"), this);
    statusBar()->addPermanentWidget(tcpLabel_);

    // ---- 模型 -> UI 接线 ----
    connect(alarmModel_.get(), &AlarmModel::alarmsChanged, this, [this] {
        alarmBar_->refresh();
        // 窗口级 ExInfoBar 弹出（左上角，默认配色与超时 4.5s）
        AlarmItem top;
        if (alarmModel_->latestSummary(top)) {
            const auto severity =
                    (top.level == AlarmLevel::Error) ? ExInfoBar::Error
                    : (top.level == AlarmLevel::Warning) ? ExInfoBar::Warning
                                                          : ExInfoBar::Informational;
            const QString title =
                    (top.level == AlarmLevel::Error)
                            ? QString::fromLocal8Bit("错误")
                    : (top.level == AlarmLevel::Warning)
                            ? QString::fromLocal8Bit("警告")
                            : QString::fromLocal8Bit("信息");
            ExInfoBarHost::defaultHost()->showInfoBar(
                    severity, title, top.summary,
                    ExInfoBarHost::TopLeft); // 默认超时 = defaultTimeout(4500ms)
        }
    }, Qt::QueuedConnection);
    connect(safety_.get(), &SafetyStateModel::stateChanged, this, [this] {
        controlVm_->onAuthorityStateChanged();
        controlArea_->refreshPermissions();
        commandPage_->refreshModeButtons();
    }, Qt::QueuedConnection);
    // 开关事务回退：窗口级提示 + 告警（ExInfoBar 弹窗经 alarmsChanged 链统一触发）
    connect(safety_.get(), &SafetyStateModel::modeRejected, this,
            [this](quint16 funcId, quint16 errCode) {
                const wire::FunctionEntry* entry =
                        wire::FunctionRegistry::findByFuncId(funcId);
                const QString name = (entry != nullptr)
                        ? QString::fromLatin1(entry->name) : QStringLiteral("unknown");
                const QString text = QString::fromLocal8Bit(
                        "开关请求被拒（%1，错误码 %2），已回退原状态")
                        .arg(name).arg(errCode);
                Logger::warning(text);
                alarmModel_->add(AlarmLevel::Warning, QStringLiteral("mode"), text);
            }, Qt::QueuedConnection);
    connect(safety_.get(), &SafetyStateModel::authorityConflict, this,
            [this](const QString& detail) {
                Logger::error(QString::fromLocal8Bit("权威状态冲突：%1").arg(detail));
                alarmModel_->add(AlarmLevel::Error, QStringLiteral("authority"), detail);
            }, Qt::QueuedConnection);

    connect(commandPage_, &CommandPageWidget::commandRequested, this,
            [this](quint16 funcId, const QByteArray& payload) {
                if (tcpClient_ != nullptr) {
                    tcpClient_->sendFrame(funcId, payload);
                }
            });
    connect(commandPage_, &CommandPageWidget::estopRequested, this, [this] {
        controlVm_->requestEstop();
    });
    connect(commandPage_, &CommandPageWidget::emergencyConfirmRequired, this,
            &MainWindow::requestEmergencyWithConfirm);

    startDataFaces();
    alarmBar_->refresh();
}

MainWindow::~MainWindow()
{
}

QWidget* MainWindow::createHomePage()
{
    const AppConfig& cfg = AppConfig::instance();

    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    auto* topSplit = new QSplitter(Qt::Horizontal, page);
    videoWidget_ = new VideoGLWidget(topSplit);
    videoWidget_->setSource(&pipeline_->displayFrames());
    topSplit->addWidget(videoWidget_);

    auto* rightColumn = new QWidget(topSplit);
    auto* rightLayout = new QVBoxLayout(rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    quick_ = new QQuickWidget(rightColumn);
    quick_->setClearColor(QColor(cfg.attitudeBackgroundColor()));
    quick_->rootContext()->setContextProperty(QStringLiteral("rovViz"), rovViz_);
    quick_->setSource(QUrl(QStringLiteral("qrc:/qml/RovViz.qml")));
    quick_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick_->setMinimumHeight(cfg.attitudeMinHeight() / 2);
    rightLayout->addWidget(quick_, 3);

    auto* sensorGroup = new QGroupBox(QString::fromLocal8Bit("传感器"), rightColumn);
    auto* form = new QFormLayout(sensorGroup);
    rollLabel_ = new QLabel(QString::fromLocal8Bit("--"), sensorGroup);
    pitchLabel_ = new QLabel(QString::fromLocal8Bit("--"), sensorGroup);
    yawLabel_ = new QLabel(QString::fromLocal8Bit("--"), sensorGroup);
    tempLabel_ = new QLabel(QString::fromLocal8Bit("--"), sensorGroup);
    humidLabel_ = new QLabel(QString::fromLocal8Bit("--"), sensorGroup);
    batteryLabel_ = new QLabel(QString::fromLocal8Bit("--"), sensorGroup);
    dypLabel_ = new QLabel(QString::fromLocal8Bit("--"), sensorGroup);
    sensorFreshLabel_ = new QLabel(QString::fromLocal8Bit("--"), sensorGroup);
    form->addRow(QString::fromLocal8Bit("横滚 Roll"), rollLabel_);
    form->addRow(QString::fromLocal8Bit("俯仰 Pitch"), pitchLabel_);
    form->addRow(QString::fromLocal8Bit("航向 Yaw"), yawLabel_);
    form->addRow(QString::fromLocal8Bit("舱内温度"), tempLabel_);
    form->addRow(QString::fromLocal8Bit("舱内湿度"), humidLabel_);
    form->addRow(QString::fromLocal8Bit("电池"), batteryLabel_);
    form->addRow(QString::fromLocal8Bit("DYP-RD 前向距离"), dypLabel_);
    form->addRow(QString::fromLocal8Bit("数据状态"), sensorFreshLabel_);
    rightLayout->addWidget(sensorGroup, 2);

    topSplit->addWidget(rightColumn);
    topSplit->setStretchFactor(0, 3);
    topSplit->setStretchFactor(1, 1);
    topSplit->setCollapsible(1, false);

    controlArea_ = new ControlAreaWidget(controlVm_.get(), safety_.get(), true,
                                         Qt::Vertical, page);

    auto* mainSplit = new QSplitter(Qt::Vertical, page);
    mainSplit->addWidget(topSplit);
    mainSplit->addWidget(controlArea_);
    mainSplit->setStretchFactor(0, 3);
    mainSplit->setStretchFactor(1, 2);
    mainSplit->setCollapsible(0, false);
    mainSplit->setCollapsible(1, false);

    pageLayout->addWidget(mainSplit);
    return page;
}

void MainWindow::startDataFaces()
{
    const AppConfig& cfg = AppConfig::instance();

    connect(pipeline_.get(), &GStreamerPipeline::errorOccurred, this,
            [this, &cfg](const QString& message) {
                statusBar()->showMessage(message, cfg.statusMessageLongMs());
                alarmModel_->add(AlarmLevel::Error, QStringLiteral("video"), message);
            },
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

    if (!pipeline_->start()) {
        statusBar()->showMessage(QString::fromLocal8Bit("视频管线启动失败，详见日志"),
                                 cfg.statusMessageErrorMs());
        alarmModel_->add(AlarmLevel::Error, QStringLiteral("video"),
                         QString::fromLocal8Bit("视频管线启动失败"));
    }

    connect(telemetryReceiver_.get(), &UdpReceiver::telemetryActiveChanged, this,
            [this](bool active) {
                telemetryLabel_->setText(active
                        ? QString::fromLocal8Bit("遥测：在线")
                        : QString::fromLocal8Bit("遥测：离线"));
            }, Qt::QueuedConnection);
    connect(telemetryReceiver_.get(), &UdpReceiver::receiverError, this,
            [this, &cfg](const QString& message) {
                statusBar()->showMessage(message, cfg.statusMessageLongMs());
            },
            Qt::QueuedConnection);
    if (cfg.telemetryUdpEnabled()) {
        telemetryReceiver_->start();
    } else {
        telemetryLabel_->setText(QString::fromLocal8Bit("遥测：UDP 回退已关闭"));
    }

    sensorModel_ = std::make_unique<SensorModel>(this);
    connect(sensorModel_.get(), &SensorModel::displayUpdated, this,
            [this] {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                const int intervalMs =
                        1000 / qMax(1, AppConfig::instance().textRefreshHz());
                if ((now - lastPanelMs_) < intervalMs) {
                    return;
                }
                lastPanelMs_ = now;

                const SensorDisplay d = sensorModel_->current();
                const int anglePrec = AppConfig::instance().anglePrecision();
                const int voltPrec = AppConfig::instance().voltagePrecision();
                const int distPrec = AppConfig::instance().distancePrecision();

                rollLabel_->setText(QString::fromLocal8Bit("%1 °")
                                        .arg(rovViz_->rollDeg(), 0, 'f', anglePrec));
                pitchLabel_->setText(QString::fromLocal8Bit("%1 °")
                                         .arg(rovViz_->pitchDeg(), 0, 'f', anglePrec));
                yawLabel_->setText(QString::fromLocal8Bit("%1 °")
                                       .arg(rovViz_->yawDeg(), 0, 'f', anglePrec));
                tempLabel_->setText(d.tempValid
                        ? QString::fromLocal8Bit("%1 °C").arg(d.tempC, 0, 'f', anglePrec)
                        : QString::fromLocal8Bit("无效"));
                humidLabel_->setText(d.humidValid
                        ? QString::fromLatin1("%1 %RH")
                              .arg(d.humidPct, 0, 'f', anglePrec)
                        : QString::fromLocal8Bit("无效"));
                if (d.voltageValid) {
                    const QString socText = d.socCalibrated
                            ? QString::fromLocal8Bit("%1%").arg(d.socPct, 0, 'f', 0)
                            : QString::fromLocal8Bit("待标定");
                    batteryLabel_->setText(
                            QString::fromLocal8Bit("%1 V|%2")
                                    .arg(d.voltage, 0, 'f', voltPrec)
                                    .arg(socText));
                } else {
                    batteryLabel_->setText(QString::fromLocal8Bit("无效"));
                }
                switch (d.dypState) {
                case DypState::Normal:
                    dypLabel_->setText(QString::fromLocal8Bit("%1 mm（正常）")
                                           .arg(d.distMm, 0, 'f', distPrec));
                    break;
                case DypState::Warning:
                    dypLabel_->setText(QString::fromLocal8Bit("%1 mm（过近警示）")
                                           .arg(d.distMm, 0, 'f', distPrec));
                    break;
                case DypState::Danger:
                    dypLabel_->setText(QString::fromLocal8Bit("%1 mm（碰撞危险）")
                                           .arg(d.distMm, 0, 'f', distPrec));
                    break;
                case DypState::OutOfRange:
                    dypLabel_->setText(QString::fromLocal8Bit("超量程"));
                    break;
                case DypState::Stale:
                    dypLabel_->setText(QString::fromLocal8Bit("数据过期"));
                    break;
                case DypState::NotReady:
                default:
                    dypLabel_->setText(QString::fromLocal8Bit("未就绪"));
                    break;
                }
                const char* srcText = (d.source == SensorDisplay::Source::Tcp) ? "TCP"
                        : (d.source == SensorDisplay::Source::Udp) ? "UDP" : "--";
                sensorFreshLabel_->setText(
                        QString::fromLocal8Bit("%1｜更新 %2s 前")
                                .arg(QString::fromLatin1(srcText))
                                .arg(d.lastUpdateMs > 0
                                             ? (now - d.lastUpdateMs) / 1000.0
                                             : 0.0,
                                     0, 'f', 1));
            },
            Qt::QueuedConnection);
    connect(sensorModel_.get(), &SensorModel::batteryAlarm, this,
            [this](bool low, bool critical) {
                if (critical) {
                    alarmModel_->add(AlarmLevel::Error, QStringLiteral("battery"),
                                     QString::fromLocal8Bit("电池严重低电量"));
                } else if (low) {
                    alarmModel_->add(AlarmLevel::Warning, QStringLiteral("battery"),
                                     QString::fromLocal8Bit("电池低电量"));
                }
            }, Qt::QueuedConnection);
    connect(&DataManager::instance(), &DataManager::rovStateUpdated, this, [this] {
        sensorModel_->applyUdpState(DataManager::instance().rovState());
    }, Qt::QueuedConnection);

    connectTcpFace();

    if (cfg.aiEnabled()) {
        aiEngine_ = std::make_unique<OnnxInferEngine>();
        connect(aiEngine_.get(), &OnnxInferEngine::backendReady, this,
                [this](const QString& backend) {
                    aiStatsLabel_->setText(
                            QString::fromLocal8Bit("AI：%1").arg(backend));
                }, Qt::QueuedConnection);
        connect(aiEngine_.get(), &OnnxInferEngine::engineFailed, this,
                [this](const QString& reason) {
                    aiStatsLabel_->setText(QString::fromLocal8Bit("AI：不可用"));
                    alarmModel_->add(AlarmLevel::Warning, QStringLiteral("ai"),
                                     QString::fromLocal8Bit("AI 不可用：%1").arg(reason));
                }, Qt::QueuedConnection);
        const int aiIntervalMs = 1000 / qMax(1, cfg.textRefreshHz());
        connect(&DataManager::instance(), &DataManager::detectionsUpdated, this,
                [this, aiIntervalMs] {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if ((now - lastAiLabelMs_) < aiIntervalMs) {
                return;
            }
            lastAiLabelMs_ = now;
            const std::size_t count = DataManager::instance().detections().size();
            aiStatsLabel_->setText(
                QString::fromLocal8Bit("AI：%1|%2ms|%3 目标")
                    .arg(aiEngine_->backendName())
                    .arg(aiEngine_->lastInferenceMs())
                    .arg(static_cast<uint>(count)));
        }, Qt::QueuedConnection);
        aiEngine_->start(&pipeline_->aiFrames());
    } else {
        aiStatsLabel_->setText(QString::fromLocal8Bit("AI：OFF（配置关闭）"));
    }
}

void MainWindow::connectTcpFace()
{
    const AppConfig& cfg = AppConfig::instance();
    if (!cfg.tcpUsable()) {
        tcpLabel_->setText(
                QString::fromLocal8Bit("TCP：禁用（配置校验未通过或已关闭）"));
        alarmModel_->add(AlarmLevel::Warning, QStringLiteral("config"),
                         QString::fromLocal8Bit("TCP 控制通道未启用"));
        return;
    }

    tcpClient_ = std::make_unique<TcpClient>();

    connect(tcpClient_.get(), &TcpClient::connectionStateChanged, this,
            [this](bool on) {
                tcpLabel_->setText(on ? QString::fromLocal8Bit("TCP：在线")
                                      : QString::fromLocal8Bit("TCP：离线"));
                safety_->setConnected(on);
                commandPage_->setLinkAvailable(on);
            }, Qt::QueuedConnection);
    connect(tcpClient_.get(), &TcpClient::clientError, this,
            [this](const QString& message) {
                Logger::warning(message);
                alarmModel_->add(AlarmLevel::Warning, QStringLiteral("tcp"), message);
            }, Qt::QueuedConnection);
    connect(tcpClient_.get(), &TcpClient::sensorSummaryReady, this,
            [this](const wire::SensorSummary& summary) {
                sensorModel_->applyTcpSummary(summary);
            }, Qt::QueuedConnection);
    connect(sensorModel_.get(), &SensorModel::attitudeReady, this,
            [this](const RovState& state) {
                DataManager::instance().setRovState(state);
            }, Qt::QueuedConnection);

    // A35 主动事件：StateEventV2 -> 权威状态（主链路）；legacy StateEvent 兼容回退；
    // AlarmEvent -> 告警中心
    connect(tcpClient_.get(), &TcpClient::eventReceived, this,
            [this](quint16 funcId, const QByteArray& payload) {
                if (funcId == static_cast<quint16>(wire::Func::StateEventV2)) {
                    quint16 mask = 0U;
                    if (wire::decodeStateEventV2(payload, mask)) {
                        safety_->applyAuthoritativeV2(mask);
                    } else {
                        alarmModel_->add(AlarmLevel::Warning, QStringLiteral("tcp"),
                                         QString::fromLocal8Bit(
                                                 "状态事件 v2 载荷非法（丢弃）"));
                    }
                } else if (funcId == static_cast<quint16>(wire::Func::StateEvent)) {
                    quint8 mask = 0U;
                    if (wire::decodeStateEvent(payload, mask)) {
                        safety_->applyAuthoritative(mask);
                    } else {
                        alarmModel_->add(AlarmLevel::Warning, QStringLiteral("tcp"),
                                         QString::fromLocal8Bit("状态事件载荷非法（丢弃）"));
                    }
                } else if (funcId == static_cast<quint16>(wire::Func::AlarmEvent)) {
                    wire::AlarmEventResult alarm;
                    if (wire::decodeAlarmEvent(payload, alarm)) {
                        const AlarmLevel level =
                                (alarm.level == 2U) ? AlarmLevel::Error
                                : (alarm.level == 1U) ? AlarmLevel::Warning
                                                      : AlarmLevel::Info;
                        alarmModel_->add(level, QStringLiteral("A35"), alarm.text,
                                         QString::fromLocal8Bit("错误码 %1")
                                                 .arg(alarm.code),
                                         0U, static_cast<qint64>(alarm.boardTimeMs));
                    } else {
                        alarmModel_->add(AlarmLevel::Warning, QStringLiteral("tcp"),
                                         QString::fromLocal8Bit("告警事件载荷非法（丢弃）"));
                    }
                } else {
                    commandPage_->onResponse(funcId, payload);
                }
            }, Qt::QueuedConnection);

    connect(tcpClient_.get(), &TcpClient::requestSent, this,
            [this](quint16 seq, quint16 funcId, const QByteArray& payload) {
                safety_->requestSent(seq, funcId);
                controlVm_->onFrameSent(seq, funcId, payload);
                commandPage_->onRequestSent(seq, funcId);
            }, Qt::QueuedConnection);
    connect(tcpClient_.get(), &TcpClient::ackReceived, this,
            [this](quint16 seq, quint16 errCode, quint16 funcId) {
                safety_->requestAcked(seq, funcId, errCode);
                controlVm_->onFrameAcked(seq, errCode);
                commandPage_->onAck(seq, errCode);
            }, Qt::QueuedConnection);
    connect(tcpClient_.get(), &TcpClient::requestTimedOut, this,
            [this](quint16 seq, quint16 funcId) {
                safety_->requestFailed(seq, funcId);
                controlVm_->onFrameFailed(seq);
                commandPage_->onTimeout(seq);
                const wire::FunctionEntry* entry =
                        wire::FunctionRegistry::findByFuncId(funcId);
                alarmModel_->add(AlarmLevel::Warning, QStringLiteral("tcp"),
                                 QString::fromLocal8Bit("请求超时（%1，seq=%2）")
                                         .arg(entry != nullptr
                                                      ? QString::fromLatin1(entry->name)
                                                      : QStringLiteral("unknown"),
                                              QString::number(seq)),
                                 QString(), seq);
            }, Qt::QueuedConnection);

    connect(controlVm_.get(), &ControlViewModel::sendRequested, this,
            [this](quint16 funcId, const QByteArray& payload) {
                tcpClient_->sendFrame(funcId, payload);
            });
    connect(controlVm_.get(), &ControlViewModel::estopRequested, this,
            [this](const QByteArray& payload) {
                tcpClient_->sendFrame(static_cast<quint16>(wire::Func::Estop), payload);
            });
    connect(controlVm_.get(), &ControlViewModel::emergencyRequested, this, [this] {
                tcpClient_->sendFrame(static_cast<quint16>(wire::Func::Emergency),
                                      QByteArray());
            });
    connect(controlVm_.get(), &ControlViewModel::permissionBlocked, this,
            [this](const QString& reason) {
                alarmModel_->add(AlarmLevel::Warning, QStringLiteral("ui"), reason);
                statusBar()->showMessage(reason,
                                         AppConfig::instance().statusMessageShortMs());
            });
    connect(controlVm_.get(), &ControlViewModel::channelUnknown, this,
            [this](int kind, int id) {
                alarmModel_->add(AlarmLevel::Warning, QStringLiteral("tcp"),
                                 QString::fromLocal8Bit("通道状态未知（超时）：%1 %2")
                                             .arg(kind == 0
                                                          ? QString::fromLocal8Bit("舵机")
                                                          : QString::fromLocal8Bit("推进器"),
                                                  QString::number(id)));
            });

    TcpClient::Settings tcpSettings;
    tcpSettings.host = cfg.tcpHost();
    tcpSettings.port = cfg.tcpPort();
    tcpSettings.connectTimeoutMs = cfg.connectTimeoutMs();
    tcpSettings.requestTimeoutMs = cfg.requestTimeoutMs();
    tcpSettings.heartbeatEnabled = cfg.heartbeatEnabled();
    tcpSettings.heartbeatIntervalMs = cfg.heartbeatIntervalMs();
    tcpSettings.reconnectEnabled = cfg.reconnectEnabled();
    tcpSettings.reconnectBaseMs = cfg.reconnectBaseMs();
    tcpSettings.reconnectMaxMs = cfg.reconnectMaxMs();
    tcpSettings.maxRetry = cfg.maxRetry();
    tcpSettings.noDelay = cfg.tcpNoDelay();
    tcpSettings.recvBufferLimit = cfg.recvBufferLimit();
    tcpSettings.maxPayload = cfg.maxPayload();
    tcpSettings.sendQueueCapacity = cfg.sendQueueCapacity();
    tcpClient_->start(tcpSettings);
}

void MainWindow::requestEmergencyWithConfirm()
{
    if (AppConfig::instance().emergencyConfirmEnabled()) {
        const auto answer = ExMessageBox::question(
                this, QString::fromLocal8Bit("紧急停机确认"),
                QString::fromLocal8Bit(
                        "确认执行紧急停机？\n（六路推进器置零；不改变舵机位置）"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    controlVm_->requestEmergency();
    statusBar()->showMessage(QString::fromLocal8Bit("紧急停机已下发"),
                             AppConfig::instance().statusMessageShortMs());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    videoWidget_->releaseGl();
    pipeline_->stopForExit();
    pipeline_.release();
    telemetryReceiver_->stop();
    if (tcpClient_ != nullptr) {
        tcpClient_->stop();
    }
    if (aiEngine_ != nullptr) {
        aiEngine_->stop();
    }
    Logger::info(QString::fromLocal8Bit("主窗口关闭：视频/遥测/TCP/推理已全部停止"));

    videoWidget_->setParent(nullptr);
    videoWidget_ = nullptr;

    event->accept();
}

} // namespace salacia
