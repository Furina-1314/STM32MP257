#include "ControlAreaWidget.h"

#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include "control/ControlViewModel.h"
#include "core/AppConfig.h"
#include "core/SafetyStateModel.h"

namespace salacia {

ControlAreaWidget::ControlAreaWidget(ControlViewModel* viewModel,
                                     SafetyStateModel* safety,
                                     bool withEmergencyArea,
                                     QWidget* parent)
    : QWidget(parent)
    , vm_(viewModel)
    , safety_(safety)
{
    auto* layout = new QHBoxLayout(this);
    servoGroup_ = buildServoGroup();
    thrusterGroup_ = buildThrusterGroup();
    layout->addWidget(servoGroup_, 4);
    layout->addWidget(thrusterGroup_, 3);
    if (withEmergencyArea) {
        layout->addWidget(buildEmergencyArea(), 2);
    }

    connect(vm_, &ControlViewModel::channelUpdated, this,
            [this](int kind, int id) {
                if (kind == 0) {
                    updateServoLabel(id);
                } else {
                    updateThrusterLabel(id);
                }
            }, Qt::QueuedConnection);
    connect(vm_, &ControlViewModel::baseUpdated, this, [this] {
        baseValue_->setText(QStringLiteral("%1%").arg(vm_->baseTarget()));
    }, Qt::QueuedConnection);

    refreshPermissions();
}

QGroupBox* ControlAreaWidget::buildServoGroup()
{
    const AppConfig& cfg = AppConfig::instance();
    auto* group = new QGroupBox(QString::fromLocal8Bit("舵机（%1 路，°）")
                                    .arg(cfg.servoCount()), this);
    auto* layout = new QHBoxLayout(group);
    layout->setSpacing(8);
    servoRows_.resize(cfg.servoCount());
    const int mid = (cfg.servoMinDeg() + cfg.servoMaxDeg()) / 2;

    for (int i = 0; i < servoRows_.size(); ++i) {
        const int id = i + 1;
        auto* column = new QVBoxLayout();
        column->setSpacing(2);
        auto* target = new QLabel(group);
        target->setAlignment(Qt::AlignCenter);
        auto* slider = new QSlider(Qt::Vertical, group);
        slider->setRange(cfg.servoMinDeg(), cfg.servoMaxDeg());
        slider->setSingleStep(cfg.servoStepDeg());
        slider->setValue(mid);
        slider->setTickPosition(QSlider::TicksBelow);
        auto* input = new QLineEdit(group);
        input->setAlignment(Qt::AlignCenter);
        input->setFixedWidth(56);
        input->setValidator(
                new QIntValidator(cfg.servoMinDeg(), cfg.servoMaxDeg(), input));
        input->installEventFilter(this);

        column->addWidget(new QLabel(QString::number(id), group), 0, Qt::AlignCenter);
        column->addWidget(target);
        column->addWidget(slider, 1);
        column->addWidget(input);
        layout->addLayout(column, 1);

        servoRows_[i].slider = slider;
        servoRows_[i].target = target;
        servoRows_[i].input = input;

        connect(slider, &QSlider::valueChanged, this, [this, id](int v) {
            vm_->setServoTarget(id, v, false);
        });
        connect(slider, &QSlider::sliderReleased, this, [this, id] {
            vm_->setServoTarget(id, servoRows_[id - 1].slider->value(), true);
        });
        connect(input, &QLineEdit::returnPressed, this, [this, id] {
            commitServoInput(id);
        });
        connect(input, &QLineEdit::editingFinished, this, [this, id] {
            commitServoInput(id);
        });
        updateServoLabel(id);
    }
    return group;
}

QGroupBox* ControlAreaWidget::buildThrusterGroup()
{
    const AppConfig& cfg = AppConfig::instance();
    auto* group = new QGroupBox(QString::fromLocal8Bit("推进器（%1 路，%）")
                                    .arg(cfg.thrusterCount()), this);
    auto* layout = new QHBoxLayout(group);
    layout->setSpacing(8);
    thrusterRows_.resize(cfg.thrusterCount());

    for (int i = 0; i < thrusterRows_.size(); ++i) {
        const int id = i + 1;
        auto* column = new QVBoxLayout();
        column->setSpacing(2);
        auto* target = new QLabel(group);
        target->setAlignment(Qt::AlignCenter);
        auto* slider = new QSlider(Qt::Vertical, group);
        slider->setRange(cfg.thrusterMinPct(), cfg.thrusterMaxPct());
        slider->setSingleStep(cfg.thrusterStepPct());
        slider->setValue(0);
        slider->setTickPosition(QSlider::TicksBelow);
        auto* input = new QLineEdit(group);
        input->setAlignment(Qt::AlignCenter);
        input->setFixedWidth(56);
        input->setValidator(
                new QIntValidator(cfg.thrusterMinPct(), cfg.thrusterMaxPct(), input));
        input->installEventFilter(this);

        column->addWidget(new QLabel(QString::number(id), group), 0, Qt::AlignCenter);
        column->addWidget(target);
        column->addWidget(slider, 1);
        column->addWidget(input);
        layout->addLayout(column, 1);

        thrusterRows_[i].slider = slider;
        thrusterRows_[i].target = target;
        thrusterRows_[i].input = input;

        connect(slider, &QSlider::valueChanged, this, [this, id](int v) {
            vm_->setThrusterTarget(id, v, false);
        });
        connect(slider, &QSlider::sliderReleased, this, [this, id] {
            vm_->setThrusterTarget(id, thrusterRows_[id - 1].slider->value(), true);
        });
        connect(input, &QLineEdit::returnPressed, this, [this, id] {
            commitThrusterInput(id);
        });
        connect(input, &QLineEdit::editingFinished, this, [this, id] {
            commitThrusterInput(id);
        });
        updateThrusterLabel(id);
    }
    return group;
}

QGroupBox* ControlAreaWidget::buildBaseGroup()
{
    const AppConfig& cfg = AppConfig::instance();
    auto* group = new QGroupBox(QString::fromLocal8Bit("基准转速（%）"), this);
    auto* layout = new QVBoxLayout(group);
    auto* row = new QHBoxLayout();
    baseSlider_ = new QSlider(Qt::Horizontal, group);
    baseSlider_->setRange(cfg.thrusterMinPct(), cfg.thrusterMaxPct());
    baseSlider_->setValue(0);
    baseValue_ = new QLabel(QStringLiteral("0%"), group);
    baseValue_->setFixedWidth(cfg.controlValueLabelWidth());
    row->addWidget(baseSlider_, 1);
    row->addWidget(baseValue_);
    layout->addLayout(row);

    connect(baseSlider_, &QSlider::valueChanged, this, [this](int v) {
        vm_->setBaseTarget(v, false);
        baseValue_->setText(QStringLiteral("%1%").arg(v));
    });
    connect(baseSlider_, &QSlider::sliderReleased, this, [this] {
        vm_->setBaseTarget(baseSlider_->value(), true);
    });
    return group;
}

QWidget* ControlAreaWidget::buildEmergencyArea()
{
    const AppConfig& cfg = AppConfig::instance();
    auto* area = new QWidget(this);
    auto* layout = new QVBoxLayout(area);

    modeHintLabel_ = new QLabel(QString::fromLocal8Bit("模式：未连接"), area);
    modeHintLabel_->setAlignment(Qt::AlignCenter);
    modeHintLabel_->setWordWrap(true);
    layout->addWidget(modeHintLabel_);

    baseGroup_ = buildBaseGroup();
    layout->addWidget(baseGroup_);

    estopBtn_ = new QPushButton(QString::fromLocal8Bit("紧急停机"), area);
    estopBtn_->setMinimumHeight(cfg.estopButtonMinHeight());
    estopBtn_->setStyleSheet(QStringLiteral(
            "QPushButton { background:%1; color:white; font-weight:bold; }"
            "QPushButton:hover { background:%2; }")
            .arg(cfg.estopButtonColor(), cfg.estopButtonHoverColor()));
    connect(estopBtn_, &QPushButton::clicked, this, [this] {
        vm_->requestEstop();
    });
    layout->addWidget(estopBtn_);

    emergencyBtn_ = new QPushButton(QString::fromLocal8Bit("紧急停机（高优先级）"), area);
    emergencyBtn_->setMinimumHeight(cfg.estopButtonMinHeight());
    emergencyBtn_->setStyleSheet(QStringLiteral(
            "QPushButton { background:#b45309; color:white; font-weight:bold; }"
            "QPushButton:hover { background:#d97706; }"));
    connect(emergencyBtn_, &QPushButton::clicked, this, [this] {
        vm_->requestEmergency();
    });
    layout->addWidget(emergencyBtn_);
    layout->addStretch(1);
    return area;
}

bool ControlAreaWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        for (const RowUi& row : servoRows_) {
            if (row.input == watched) {
                return true;
            }
        }
        for (const RowUi& row : thrusterRows_) {
            if (row.input == watched) {
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ControlAreaWidget::commitServoInput(int id)
{
    const ChannelVm ch = vm_->servo(id);
    const QString text = servoRows_[id - 1].input->text().trimmed();
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || text.isEmpty()) {
        servoRows_[id - 1].input->setText(QString::number(ch.target));
        return;
    }
    vm_->setServoTarget(id, value, true);
    servoRows_[id - 1].input->setText(QString::number(vm_->servo(id).target));
}

void ControlAreaWidget::commitThrusterInput(int id)
{
    const ChannelVm ch = vm_->thruster(id);
    const QString text = thrusterRows_[id - 1].input->text().trimmed();
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || text.isEmpty()) {
        thrusterRows_[id - 1].input->setText(QString::number(ch.target));
        return;
    }
    vm_->setThrusterTarget(id, value, true);
    thrusterRows_[id - 1].input->setText(QString::number(vm_->thruster(id).target));
}

void ControlAreaWidget::updateServoLabel(int id)
{
    const ChannelVm ch = vm_->servo(id);
    const QString confirmed = ch.confirmedValid
            ? QString::number(ch.confirmed) : QString::fromLocal8Bit("?");
    servoRows_[id - 1].target->setText(
            QString::fromLocal8Bit("%1°|%2").arg(ch.target).arg(confirmed));
    servoRows_[id - 1].slider->setValue(ch.target);
    if (!servoRows_[id - 1].input->hasFocus()) {
        servoRows_[id - 1].input->setText(QString::number(ch.target));
    }
}

void ControlAreaWidget::updateThrusterLabel(int id)
{
    const ChannelVm ch = vm_->thruster(id);
    const QString confirmed = ch.confirmedValid
            ? QString::number(ch.confirmed) : QString::fromLocal8Bit("?");
    thrusterRows_[id - 1].target->setText(
            QString::fromLocal8Bit("%1%%2").arg(ch.target).arg(confirmed));
    thrusterRows_[id - 1].slider->setValue(ch.target);
    if (!thrusterRows_[id - 1].input->hasFocus()) {
        thrusterRows_[id - 1].input->setText(QString::number(ch.target));
    }
}

void ControlAreaWidget::refreshPermissions()
{
    const bool servo = safety_->canServoIndividual();
    const bool thruster = safety_->canThrusterIndividual();
    const bool base = safety_->canBaseSlider();

    servoGroup_->setEnabled(servo);
    thrusterGroup_->setEnabled(thruster);
    thrusterGroup_->setVisible(!safety_->baseSliderVisible());
    if (baseGroup_ != nullptr) {
        baseGroup_->setVisible(safety_->baseSliderVisible());
        baseGroup_->setEnabled(base);
    }
    if (modeHintLabel_ != nullptr) {
        modeHintLabel_->setText(safety_->controlsLocked()
                ? QString::fromLocal8Bit("等待权威状态\n（断线或未收到状态事件）")
                : (safety_->safeState() == ModeState::On
                           ? QString::fromLocal8Bit("安全模式已开启")
                           : safety_->horizontalState() == ModeState::On
                                     ? QString::fromLocal8Bit("姿态稳定已开启")
                                     : QString::fromLocal8Bit("普通手动")));
    }
    if (estopBtn_ != nullptr) {
        switch (safety_->estopButton()) {
        case EmergencyButtonState::Disabled:
            estopBtn_->setEnabled(false);
            estopBtn_->setText(QString::fromLocal8Bit("紧急停机（无法下发）"));
            break;
        case EmergencyButtonState::Triggered:
            estopBtn_->setEnabled(true);
            estopBtn_->setText(QString::fromLocal8Bit("紧急停机（已触发）"));
            break;
        default:
            estopBtn_->setEnabled(true);
            estopBtn_->setText(QString::fromLocal8Bit("紧急停机"));
            break;
        }
    }
    if (emergencyBtn_ != nullptr) {
        switch (safety_->emergencyButton()) {
        case EmergencyButtonState::Disabled:
            emergencyBtn_->setEnabled(false);
            emergencyBtn_->setText(QString::fromLocal8Bit("紧急停机·高（无法下发）"));
            break;
        case EmergencyButtonState::InProgress:
            emergencyBtn_->setEnabled(true);
            emergencyBtn_->setText(QString::fromLocal8Bit("紧急停机·高（进行中）"));
            break;
        default:
            emergencyBtn_->setEnabled(true);
            emergencyBtn_->setText(QString::fromLocal8Bit("紧急停机（高优先级）"));
            break;
        }
    }
}

} // namespace salacia
