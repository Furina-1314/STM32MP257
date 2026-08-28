#include "ControlPanelWidget.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include "core/AppConfig.h"
#include "core/Logger.h"

namespace salacia {

namespace {
constexpr int kServoChannels = 10;
constexpr int kThrusterChannels = 6;
constexpr int kFlushIntervalMs = 50; // 合并节拍（指令延迟红线 ≤50ms）
constexpr int kServoDefaultDeg = 90;
} // namespace

ControlPanelWidget::ControlPanelWidget(QWidget* parent)
    : QWidget(parent)
{
    const AppConfig& cfg = AppConfig::instance();
    servoMinUs_ = cfg.servoMinUs();
    servoMaxUs_ = cfg.servoMaxUs();
    thrusterMinUs_ = cfg.thrusterMinUs();
    thrusterMaxUs_ = cfg.thrusterMaxUs();
    thrusterNeutralUs_ = cfg.thrusterNeutralUs();

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(buildServoGroup());
    layout->addWidget(buildThrusterGroup());
    layout->addWidget(buildFooter());
    layout->addStretch(1);

    flushTimer_.setInterval(kFlushIntervalMs);
    connect(&flushTimer_, &QTimer::timeout, this, &ControlPanelWidget::flushPending);
    flushTimer_.start();
}

QGroupBox* ControlPanelWidget::buildServoGroup()
{
    auto* group = new QGroupBox(QString::fromLocal8Bit("舵机（通道 1-10）"), this);
    auto* grid = new QVBoxLayout(group);

    for (int i = 0; i < kServoChannels; ++i) {
        const int channel = i + 1; // PWM 通道 = 1..10
        auto* row = new QHBoxLayout();
        auto* name = new QLabel(QString::fromLocal8Bit("舵机 %1").arg(channel), group);
        name->setFixedWidth(64);
        auto* slider = new QSlider(Qt::Horizontal, group);
        slider->setRange(0, 180);
        slider->setValue(kServoDefaultDeg);
        auto* value = new QLabel(group);
        value->setFixedWidth(110);

        row->addWidget(name);
        row->addWidget(slider, 1);
        row->addWidget(value);
        grid->addLayout(row);

        servoRows_[i].slider = slider;
        servoRows_[i].value = value;

        connect(slider, &QSlider::valueChanged, this, [this, channel] {
            updateServoLabel(channel);
            queueServo(channel);
        });
        updateServoLabel(channel);
    }
    return group;
}

QGroupBox* ControlPanelWidget::buildThrusterGroup()
{
    auto* group = new QGroupBox(QString::fromLocal8Bit("推进器（通道 11-16）"), this);
    auto* grid = new QVBoxLayout(group);

    for (int i = 0; i < kThrusterChannels; ++i) {
        const int channel = kServoChannels + i + 1; // PWM 通道 = 11..16
        auto* row = new QHBoxLayout();
        auto* name = new QLabel(QString::fromLocal8Bit("推进 %1").arg(i + 1), group);
        name->setFixedWidth(64);
        auto* slider = new QSlider(Qt::Horizontal, group);
        slider->setRange(-100, 100);
        slider->setValue(0);
        auto* neutral = new QPushButton(QString::fromLocal8Bit("中位"), group);
        auto* value = new QLabel(group);
        value->setFixedWidth(110);

        row->addWidget(name);
        row->addWidget(slider, 1);
        row->addWidget(neutral);
        row->addWidget(value);
        grid->addLayout(row);

        thrusterRows_[i].slider = slider;
        thrusterRows_[i].value = value;

        connect(slider, &QSlider::valueChanged, this, [this, channel] {
            updateThrusterLabel(channel - kServoChannels);
            queueThruster(channel - kServoChannels);
        });
        connect(neutral, &QPushButton::clicked, slider, [slider] {
            slider->setValue(0); // 触发 valueChanged -> 正常下发链路
        });
        updateThrusterLabel(i + 1);
    }
    return group;
}

QWidget* ControlPanelWidget::buildFooter()
{
    auto* footer = new QWidget(this);
    auto* layout = new QVBoxLayout(footer);
    layout->setContentsMargins(0, 4, 0, 0);

    linkHint_ = new QLabel(QString::fromLocal8Bit("SSH：离线（指令排队，连接后自动丢弃过期值）"), footer);
    linkHint_->setStyleSheet(QStringLiteral("color:#c0a040;"));
    layout->addWidget(linkHint_);

    auto* emergency = new QPushButton(QString::fromLocal8Bit("紧急停机（推进器中位 / 舵机回中）"), footer);
    emergency->setMinimumHeight(42);
    emergency->setStyleSheet(QStringLiteral(
            "QPushButton { background:#8c2f2f; color:white; font-weight:bold; }"
            "QPushButton:hover { background:#a83a3a; }"));
    connect(emergency, &QPushButton::clicked, this, &ControlPanelWidget::emergencyStop);
    layout->addWidget(emergency);
    return footer;
}

void ControlPanelWidget::setLinkStatus(bool connected)
{
    if (connected) {
        linkHint_->setText(QString::fromLocal8Bit("SSH：在线"));
        linkHint_->setStyleSheet(QStringLiteral("color:#3ddc84;"));
    } else {
        linkHint_->setText(QString::fromLocal8Bit("SSH：离线（指令排队，连接后自动丢弃过期值）"));
        linkHint_->setStyleSheet(QStringLiteral("color:#c0a040;"));
    }
}

int ControlPanelWidget::servoUs(int channel) const
{
    const int deg = servoRows_[channel - 1].slider->value();
    return servoMinUs_ + static_cast<int>(deg * (servoMaxUs_ - servoMinUs_) / 180.0);
}

int ControlPanelWidget::thrusterUs(int channel) const
{
    const int pct = thrusterRows_[channel - 1].slider->value();
    return thrusterNeutralUs_
            + static_cast<int>(pct * (thrusterMaxUs_ - thrusterMinUs_) / 200.0);
}

void ControlPanelWidget::updateServoLabel(int channel)
{
    servoRows_[channel - 1].value->setText(
            QString::fromLocal8Bit("%1°｜%2 us")
                .arg(servoRows_[channel - 1].slider->value())
                .arg(servoUs(channel)));
}

void ControlPanelWidget::updateThrusterLabel(int channel)
{
    const int pct = thrusterRows_[channel - 1].slider->value();
    const QString sign = (pct > 0) ? QStringLiteral("+") : QString();
    thrusterRows_[channel - 1].value->setText(
            QString::fromLocal8Bit("%1%2%｜%3 us")
                .arg(sign)
                .arg(pct)
                .arg(thrusterUs(channel)));
}

void ControlPanelWidget::queueServo(int channel)
{
    pending_.insert(channel, servoUs(channel));
}

void ControlPanelWidget::queueThruster(int channel)
{
    pending_.insert(kServoChannels + channel, thrusterUs(channel));
}

void ControlPanelWidget::flushPending()
{
    if (pending_.isEmpty()) {
        return;
    }
    const QMap<int, int> out = pending_;
    pending_.clear();
    for (auto it = out.constBegin(); it != out.constEnd(); ++it) {
        emit pwmCommandRequested(it.key(), it.value());
    }
}

void ControlPanelWidget::emergencyStop()
{
    Logger::warning(QString::fromLocal8Bit("遥控：紧急停机触发（推进器中位/舵机回中）"));
    for (int i = 0; i < kThrusterChannels; ++i) {
        thrusterRows_[i].slider->setValue(0); // valueChanged -> 正常链路
    }
    for (int i = 0; i < kServoChannels; ++i) {
        servoRows_[i].slider->setValue(kServoDefaultDeg);
    }
    // 立即冲刷，绕过合并节拍（安全优先）
    flushPending();
    emit emergencyStopRequested();
}

} // namespace salacia
