#include "SettingsPageWidget.h"

#include <QApplication>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetList>

#include "core/AppConfig.h"
#include "core/Logger.h"

namespace salacia {

namespace {
constexpr const char* kAccentPresets[] = {
    "#0078D4", "#0063B1", "#8E8CD8", "#6B69D6", "#8764B8", "#744DA9",
    "#B146C2", "#881798", "#E3008C", "#C30052", "#E74856", "#EA005E",
    "#0099BC", "#2D7D9A", "#00B7C3", "#038387", "#00B294", "#018574",
    "#00CC6A", "#10893E", "#7A7574", "#5D5A58", "#68768A", "#515C6B",
};
constexpr int kAccentPresetCount = sizeof(kAccentPresets) / sizeof(kAccentPresets[0]);
} // namespace

SettingsPageWidget::SettingsPageWidget(AppConfig& cfg, QWidget* parent)
    : QWidget(parent)
    , cfg_(cfg)
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->addWidget(buildAccentGroup(), 0);
    rootLayout->addWidget(buildParamsGroup(), 1);
    rootLayout->addWidget(buildSummaryGroup(), 0);
    rootLayout->addStretch(1);

    const QString conf = cfg_.uiAccentColor();
    if (!conf.isEmpty()) {
        currentAccent_ = QColor(conf);
    }
}

QWidget* SettingsPageWidget::buildAccentGroup()
{
    auto* group = new QGroupBox(QString::fromLocal8Bit("强调色（即时生效）"), this);
    auto* layout = new QVBoxLayout(group);
    auto* grid = new QGridLayout();
    grid->setSpacing(6);
    for (int i = 0; i < kAccentPresetCount; ++i) {
        const QColor color(QString::fromLatin1(kAccentPresets[i]));
        auto* swatch = new QPushButton(group);
        swatch->setFixedSize(40, 40);
        swatch->setToolTip(color.name().toUpper());
        swatch->setStyleSheet(QStringLiteral(
                "QPushButton { background:%1; border:2px solid transparent;"
                "border-radius:4px; }"
                "QPushButton:hover { border:2px solid #888888; }").arg(color.name()));
        grid->addWidget(swatch, i / 8, i % 8);
        accentSwatches_.append(swatch);
        connect(swatch, &QPushButton::clicked, this, [this, color] {
            applyAccent(color);
        });
    }
    customAccentBtn_ = new QPushButton(QString::fromLocal8Bit("自定义颜色..."), group);
    grid->addWidget(customAccentBtn_, 0, 8, 2, 1);
    layout->addLayout(grid);

    connect(customAccentBtn_, &QPushButton::clicked, this, [this] {
        const QColor picked = QColorDialog::getColor(
                currentAccent_, this, QString::fromLocal8Bit("选择强调色"));
        if (picked.isValid()) {
            applyAccent(picked);
        }
    });
    return group;
}

void SettingsPageWidget::applyAccent(const QColor& color)
{
    currentAccent_ = color;
    QPalette pal = qApp->palette();
    pal.setColor(QPalette::Accent, color);
    pal.setColor(QPalette::Highlight, color);
    qApp->setPalette(pal);
    const QWidgetList widgets = qApp->allWidgets();
    for (QWidget* w : widgets) {
        w->style()->unpolish(w);
        w->style()->polish(w);
    }
    Logger::info(QString::fromLocal8Bit("设置：强调色切换为 %1").arg(color.name().toUpper()));
}

QWidget* SettingsPageWidget::buildParamsGroup()
{
    auto* group = new QGroupBox(
            QString::fromLocal8Bit("运行参数（编辑即时生效；\"保存配置\"写入 ini）"), this);
    auto* form = new QGridLayout(group);
    int row = 0;

    const auto addDouble = [this, &form, &row, group](const QString& label,
                                               QDoubleSpinBox*& spin, double value,
                                               double lo, double hi, int decimals,
                                               double step) {
        form->addWidget(new QLabel(label, group), row, 0);
        spin = new QDoubleSpinBox(group);
        spin->setRange(lo, hi);
        spin->setDecimals(decimals);
        spin->setSingleStep(step);
        spin->setValue(value);
        form->addWidget(spin, row, 1);
        ++row;
    };
    const auto addInt = [this, &form, &row, group](const QString& label, QSpinBox*& spin,
                                            int value, int lo, int hi) {
        form->addWidget(new QLabel(label, group), row, 0);
        spin = new QSpinBox(group);
        spin->setRange(lo, hi);
        spin->setValue(value);
        form->addWidget(spin, row, 1);
        ++row;
    };

    addDouble(QString::fromLocal8Bit("AI 置信度阈值"), confSpin_,
              cfg_.confidenceThreshold(), 0.0, 1.0, 2, 0.05);
    addDouble(QString::fromLocal8Bit("AI NMS IoU 阈值"), nmsSpin_,
              cfg_.nmsIouThreshold(), 0.0, 1.0, 2, 0.05);
    addInt(QString::fromLocal8Bit("TCP 请求超时 [ms]"), reqTimeoutSpin_,
           cfg_.requestTimeoutMs(), 100, 60000);
    addInt(QString::fromLocal8Bit("TCP 心跳周期 [ms]"), heartbeatSpin_,
           cfg_.heartbeatIntervalMs(), 100, 60000);
    addInt(QString::fromLocal8Bit("传感器过期阈值 [ms]"), sensorStaleSpin_,
           cfg_.sensorStaleMs(), 50, 10000);
    addInt(QString::fromLocal8Bit("告警条数上限"), alarmMaxSpin_,
           cfg_.alarmMaxItems(), 10, 10000);
    addInt(QString::fromLocal8Bit("告警合并窗口 [ms]"), alarmMergeSpin_,
           cfg_.alarmMergeWindowMs(), 0, 600000);
    addInt(QString::fromLocal8Bit("角度显示精度"), anglePrecSpin_,
           cfg_.anglePrecision(), 0, 3);
    addInt(QString::fromLocal8Bit("电压显示精度"), voltagePrecSpin_,
           cfg_.voltagePrecision(), 0, 3);
    addInt(QString::fromLocal8Bit("距离显示精度"), distancePrecSpin_,
           cfg_.distancePrecision(), 0, 3);
    addDouble(QString::fromLocal8Bit("电池低电量阈值 [%]"), batteryLowSpin_,
              cfg_.batteryLowThresholdPct(), 0.0, 100.0, 1, 1.0);
    addDouble(QString::fromLocal8Bit("电池严重低电量阈值 [%]"), batteryCriticalSpin_,
              cfg_.batteryCriticalThresholdPct(), 0.0, 100.0, 1, 1.0);

    connect(confSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        cfg_.setConfidenceThreshold(v);
    });
    connect(nmsSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        cfg_.setNmsIouThreshold(v);
    });
    connect(reqTimeoutSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        cfg_.setRequestTimeoutMs(v);
    });
    connect(heartbeatSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        cfg_.setHeartbeatIntervalMs(v);
    });
    connect(sensorStaleSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        cfg_.setSensorStaleMs(v);
    });
    connect(alarmMaxSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        cfg_.setAlarmMaxItems(v);
    });
    connect(alarmMergeSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        cfg_.setAlarmMergeWindowMs(v);
    });
    connect(anglePrecSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        cfg_.setAnglePrecision(v);
    });
    connect(voltagePrecSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        cfg_.setVoltagePrecision(v);
    });
    connect(distancePrecSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        cfg_.setDistancePrecision(v);
    });
    connect(batteryLowSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        cfg_.setBatteryLowPct(v);
    });
    connect(batteryCriticalSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        cfg_.setBatteryCriticalPct(v);
    });

    saveBtn_ = new QPushButton(QString::fromLocal8Bit("保存配置（写入 ini）"), group);
    saveBtn_->setMinimumHeight(40);
    form->addWidget(saveBtn_, row, 0, 1, 2);
    connect(saveBtn_, &QPushButton::clicked, this, &SettingsPageWidget::saveToIni);

    QWidget* spacer = new QWidget(group);
    form->addWidget(spacer, 0, 2, row + 1, 1);
    form->setColumnStretch(2, 1);
    return group;
}

void SettingsPageWidget::saveToIni()
{
    const QString path = cfg_.iniPath();
    if (path.isEmpty()) {
        Logger::warning(QString::fromLocal8Bit(
                "设置：未找到配置文件路径，无法保存"));
        return;
    }
    QSettings ini(path, QSettings::IniFormat);
    ini.setValue(QStringLiteral("ai/confidence_threshold"), confSpin_->value());
    ini.setValue(QStringLiteral("ai/nms_iou_threshold"), nmsSpin_->value());
    ini.setValue(QStringLiteral("tcp/request_timeout_ms"), reqTimeoutSpin_->value());
    ini.setValue(QStringLiteral("tcp/heartbeat_interval_ms"), heartbeatSpin_->value());
    ini.setValue(QStringLiteral("tcp/sensor_stale_ms"), sensorStaleSpin_->value());
    ini.setValue(QStringLiteral("alarms/max_items"), alarmMaxSpin_->value());
    ini.setValue(QStringLiteral("alarms/merge_window_ms"), alarmMergeSpin_->value());
    ini.setValue(QStringLiteral("ui/angle_precision"), anglePrecSpin_->value());
    ini.setValue(QStringLiteral("ui/voltage_precision"), voltagePrecSpin_->value());
    ini.setValue(QStringLiteral("ui/distance_precision"), distancePrecSpin_->value());
    ini.setValue(QStringLiteral("battery/low_threshold_pct"), batteryLowSpin_->value());
    ini.setValue(QStringLiteral("battery/critical_threshold_pct"),
                 batteryCriticalSpin_->value());
    ini.setValue(QStringLiteral("ui/accent_color"),
                 currentAccent_.isValid() ? currentAccent_.name() : QString());
    ini.sync();
    Logger::info(QString::fromLocal8Bit("设置：配置已保存至 %1").arg(path));
    emit configSaved();
}

QWidget* SettingsPageWidget::buildSummaryGroup()
{
    auto* group = new QGroupBox(
            QString::fromLocal8Bit("启动参数摘要（网络/端口/模型路径修改 ini 后重启生效）"),
            this);
    auto* layout = new QVBoxLayout(group);
    summaryLabel_ = new QLabel(group);
    summaryLabel_->setWordWrap(true);
    summaryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(summaryLabel_);

    summaryLabel_->setText(QString::fromLocal8Bit(
            "网络：主机 %1｜板端 %2｜视频 RTP %3｜遥测 UDP %4（回退 %5）\n"
            "TCP：%6:%7｜视频解码 %8｜抖动 %9ms\n"
            "AI：模型 %10（%11）\n"
            "配置文件：%12")
            .arg(cfg_.hostIp().isEmpty() ? QStringLiteral("0.0.0.0") : cfg_.hostIp(),
                 cfg_.boardIp())
            .arg(cfg_.videoRtpPort())
            .arg(cfg_.telemetryPort())
            .arg(cfg_.telemetryUdpEnabled() ? QString::fromLocal8Bit("开")
                                           : QString::fromLocal8Bit("关"))
            .arg(cfg_.tcpUsable() ? cfg_.tcpHost() : QString::fromLocal8Bit("未启用"))
            .arg(cfg_.tcpUsable() ? QString::number(cfg_.tcpPort())
                                  : QString::fromLocal8Bit("-"))
            .arg(cfg_.preferredDecoder())
            .arg(cfg_.jitterLatencyMs())
            .arg(cfg_.modelPath(), cfg_.executionProvider())
            .arg(cfg_.iniPath().isEmpty() ? QString::fromLocal8Bit("（内置默认）")
                                         : cfg_.iniPath()));

    auto* openBtn = new QPushButton(QString::fromLocal8Bit("打开配置目录"), group);
    layout->addWidget(openBtn);
    connect(openBtn, &QPushButton::clicked, this, [this] {
        const QString dir = QFileInfo(cfg_.iniPath()).absolutePath();
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
            Logger::warning(QString::fromLocal8Bit("设置：无法打开配置目录 %1").arg(dir));
        }
    });
    return group;
}

} // namespace salacia
