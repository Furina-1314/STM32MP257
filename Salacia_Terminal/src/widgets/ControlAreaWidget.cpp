#include "ControlAreaWidget.h"

#include <QEvent>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QStackedLayout>
#include <QVBoxLayout>

#include "communication/WireConstants.h"
#include "control/ControlViewModel.h"
#include "core/AppConfig.h"
#include "core/SafetyStateModel.h"
#include "widgets/SwitchButtonWidget.h"

namespace salacia {

ControlAreaWidget::ControlAreaWidget(ControlViewModel* viewModel,
                                     SafetyStateModel* safety,
                                     bool withEmergencyArea,
                                     Qt::Orientation sliderOrientation,
                                     QWidget* parent)
    : QWidget(parent)
    , vm_(viewModel)
    , safety_(safety)
    , orientation_(sliderOrientation)
{
    if (orientation_ == Qt::Horizontal) {
        // 指令页（水平滑条）：舵机 5x2 / 垂直 2x2 / 水平 2x1 三个模块自上而下
        // 占满整个控制区（顶到窗口最右侧），使能开关作为底部一行不另占列
        auto* layout = new QVBoxLayout(this);
        servoGroup_ = buildServoGroup();
        layout->addWidget(servoGroup_, 2);
        layout->addWidget(buildThrusterGroup(true), 2);
        layout->addWidget(buildThrusterGroup(false), 1);
        layout->addWidget(buildEnableGroup());
    } else {
        // 主页（竖直滑条）：舵机 | 垂直 | 水平 | 右列（使能+紧急固定区）
        auto* layout = new QHBoxLayout(this);
        servoGroup_ = buildServoGroup();
        layout->addWidget(servoGroup_, 4);
        layout->addWidget(buildThrusterGroup(true), 3);
        layout->addWidget(buildThrusterGroup(false), 2);

        auto* rightColumn = new QVBoxLayout();
        rightColumn->addWidget(buildEnableGroup());
        if (withEmergencyArea) {
            rightColumn->addWidget(buildEmergencyArea(), 1);
        } else {
            rightColumn->addStretch(1);
        }
        layout->addLayout(rightColumn, 2);
    }

    connect(vm_, &ControlViewModel::channelUpdated, this,
            [this](int kind, int id) {
                if (kind == 0) {
                    updateServoLabel(id);
                } else {
                    // 扁平 1..4=垂直 5..6=水平
                    if (id <= wire::kVerticalCount) {
                        updateThrusterLabel(true, id);
                    } else {
                        updateThrusterLabel(false, id - wire::kVerticalCount);
                    }
                }
            }, Qt::QueuedConnection);
    connect(vm_, &ControlViewModel::baseUpdated, this, [this] {
        updateBaseUi(true);
        updateBaseUi(false);
    }, Qt::QueuedConnection);
    // 请求被拒（链路不可用等）：乐观显示回到权威状态
    connect(vm_, &ControlViewModel::permissionBlocked, this,
            &ControlAreaWidget::refreshPermissions, Qt::QueuedConnection);

    refreshPermissions();
}

// ---------------------------------------------------------------- 舵机组

QGroupBox* ControlAreaWidget::buildServoGroup()
{
    const AppConfig& cfg = AppConfig::instance();
    auto* group = new QGroupBox(QString::fromLocal8Bit("舵机（%1 路，°）")
                                    .arg(wire::kServoCount), this);
    // 主页竖直滑条（列布局）；指令页水平滑条（5 列 x 2 行网格，两行式器件格）
    QGridLayout* grid = nullptr;
    QBoxLayout* layout = nullptr;
    const int gridCols = wire::kServoCount / 2; // 10 -> 5 列
    if (orientation_ == Qt::Horizontal) {
        grid = new QGridLayout(group);
        grid->setSpacing(6);
    } else {
        layout = new QHBoxLayout(group);
        layout->setSpacing(8);
    }
    servoRows_.resize(wire::kServoCount);
    const int mid = (cfg.servoMinDeg() + cfg.servoMaxDeg()) / 2;

    for (int i = 0; i < servoRows_.size(); ++i) {
        const int uiNumber = i + 1;
        auto* target = new QLabel(group);
        target->setAlignment(Qt::AlignCenter);
        auto* slider = new QSlider(orientation_, group);
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
        // 标签规范：舵机1（CH0）..舵机10（CH9）——UI 编号与 wireId 分离
        auto* caption = new QLabel(QString::fromLocal8Bit("舵机%1（CH%2）")
                                           .arg(uiNumber)
                                           .arg(wire::servoWireId(uiNumber)),
                                   group);

        if (orientation_ == Qt::Horizontal) {
            // 两行式器件格：[标签 三态 输入框] / [水平滑条占满格宽]
            caption->setFixedWidth(104);
            target->setFixedWidth(60);
            auto* cell = new QVBoxLayout();
            cell->setSpacing(2);
            auto* info = new QHBoxLayout();
            info->setSpacing(4);
            info->addWidget(caption);
            info->addWidget(target, 1);
            info->addWidget(input);
            cell->addLayout(info);
            cell->addWidget(slider, 1);
            grid->addLayout(cell, i / gridCols, i % gridCols);
        } else {
            caption->setAlignment(Qt::AlignCenter);
            auto* column = new QVBoxLayout();
            column->setSpacing(2);
            column->addWidget(caption, 0, Qt::AlignCenter);
            column->addWidget(target);
            column->addWidget(slider, 1);
            column->addWidget(input);
            layout->addLayout(column, 1);
        }

        servoRows_[i].slider = slider;
        servoRows_[i].target = target;
        servoRows_[i].input = input;

        connect(slider, &QSlider::valueChanged, this, [this, uiNumber](int v) {
            vm_->setServoTarget(uiNumber, v, false);
        });
        connect(slider, &QSlider::sliderReleased, this, [this, uiNumber] {
            vm_->setServoTarget(uiNumber,
                                servoRows_[uiNumber - 1].slider->value(), true);
        });
        connect(input, &QLineEdit::returnPressed, this, [this, uiNumber] {
            commitServoInput(uiNumber);
        });
        connect(input, &QLineEdit::editingFinished, this, [this, uiNumber] {
            commitServoInput(uiNumber);
        });
        updateServoLabel(uiNumber);
    }
    return group;
}

// ---------------------------------------------------------------- 推进器组

QGroupBox* ControlAreaWidget::buildThrusterGroup(bool vertical)
{
    const AppConfig& cfg = AppConfig::instance();
    const int count = vertical ? wire::kVerticalCount : wire::kHorizontalCount;
    GroupUi& g = vertical ? verticalGroup_ : horizontalGroup_;
    g.group = new QGroupBox(vertical
            ? QString::fromLocal8Bit("垂直推进器（CH10-13，%）")
            : QString::fromLocal8Bit("水平推进器（CH14-15，%）"), this);
    auto* vlay = new QVBoxLayout(g.group);

    // 组同步开关（权威显示可切换；姿态稳定 ON 时布局仍保持一组一条）
    g.syncSwitch = new SwitchButtonWidget(
            vertical ? QString::fromLocal8Bit("垂直同步（Synchronization）")
                     : QString::fromLocal8Bit("水平同步（Synchronization）"),
            g.group);
    vlay->addWidget(g.syncSwitch);
    connect(g.syncSwitch, &SwitchButtonWidget::toggleRequested, this,
            [this, vertical](bool on) {
                SwitchButtonWidget* sw = (vertical ? verticalGroup_ : horizontalGroup_)
                                                   .syncSwitch;
                sw->showPending(on); // 乐观显示，回退经刷新链修正
                onSyncSwitchToggled(vertical, on);
            });

    // 布局堆叠：0=独立 1=同步 2=基准
    auto* stackHost = new QWidget(g.group);
    g.stack = new QStackedLayout(stackHost);

    // 独立滑条页（主页竖直列布局；指令页 2 列网格、两行式器件格）
    auto* indPage = new QWidget(stackHost);
    QGridLayout* indGrid = nullptr;
    QBoxLayout* indLay = nullptr;
    const int gridCols = qMin(count, 2); // 垂直 2x2 / 水平 2x1
    if (orientation_ == Qt::Horizontal) {
        indGrid = new QGridLayout(indPage);
        indGrid->setSpacing(6);
    } else {
        indLay = new QHBoxLayout(indPage);
        indLay->setSpacing(8);
    }
    g.rows.resize(count);
    for (int i = 0; i < count; ++i) {
        const int uiNumber = i + 1;
        auto* target = new QLabel(indPage);
        target->setAlignment(Qt::AlignCenter);
        auto* slider = new QSlider(orientation_, indPage);
        slider->setRange(cfg.thrusterMinPct(), cfg.thrusterMaxPct());
        slider->setSingleStep(cfg.thrusterStepPct());
        slider->setValue(0);
        slider->setTickPosition(QSlider::TicksBelow);
        auto* input = new QLineEdit(indPage);
        input->setAlignment(Qt::AlignCenter);
        input->setFixedWidth(56);
        input->setValidator(
                new QIntValidator(cfg.thrusterMinPct(), cfg.thrusterMaxPct(), input));
        input->installEventFilter(this);

        // 标签规范：垂直1（CH10）..水平2（CH15）
        const quint8 wireId = vertical ? wire::verticalWireId(uiNumber)
                                       : wire::horizontalWireId(uiNumber);
        auto* caption = new QLabel(
                QString::fromLocal8Bit(vertical ? "垂直%1（CH%2）" : "水平%1（CH%2）")
                        .arg(uiNumber).arg(wireId), indPage);

        if (orientation_ == Qt::Horizontal) {
            // 两行式器件格：[标签 三态 输入框] / [水平滑条占满格宽]
            caption->setFixedWidth(104);
            target->setFixedWidth(60);
            auto* cell = new QVBoxLayout();
            cell->setSpacing(2);
            auto* info = new QHBoxLayout();
            info->setSpacing(4);
            info->addWidget(caption);
            info->addWidget(target, 1);
            info->addWidget(input);
            cell->addLayout(info);
            cell->addWidget(slider, 1);
            indGrid->addLayout(cell, i / gridCols, i % gridCols);
        } else {
            caption->setAlignment(Qt::AlignCenter);
            auto* column = new QVBoxLayout();
            column->setSpacing(2);
            column->addWidget(caption, 0, Qt::AlignCenter);
            column->addWidget(target);
            column->addWidget(slider, 1);
            column->addWidget(input);
            indLay->addLayout(column, 1);
        }

        g.rows[i].slider = slider;
        g.rows[i].target = target;
        g.rows[i].input = input;

        connect(slider, &QSlider::valueChanged, this,
                [this, vertical, uiNumber](int v) {
            if (vertical) {
                vm_->setVerticalThrusterTarget(uiNumber, v, false);
            } else {
                vm_->setHorizontalThrusterTarget(uiNumber, v, false);
            }
        });
        connect(slider, &QSlider::sliderReleased, this,
                [this, vertical, uiNumber] {
            const QSlider* s = (vertical ? verticalGroup_ : horizontalGroup_)
                                       .rows.at(uiNumber - 1).slider;
            if (vertical) {
                vm_->setVerticalThrusterTarget(uiNumber, s->value(), true);
            } else {
                vm_->setHorizontalThrusterTarget(uiNumber, s->value(), true);
            }
        });
        connect(input, &QLineEdit::returnPressed, this,
                [this, vertical, uiNumber] { commitThrusterInput(vertical, uiNumber); });
        connect(input, &QLineEdit::editingFinished, this,
                [this, vertical, uiNumber] { commitThrusterInput(vertical, uiNumber); });
        updateThrusterLabel(vertical, uiNumber);
    }
    g.stack->addWidget(indPage);

    // 同步滑条页（单条控制全组同值；方向跟随组方向）
    auto* syncPage = new QWidget(stackHost);
    auto* syncLay = new QHBoxLayout(syncPage);
    syncLay->setSpacing(8);
    g.syncTarget = new QLabel(syncPage);
    g.syncTarget->setAlignment(Qt::AlignCenter);
    g.syncSlider = new QSlider(orientation_, syncPage);
    g.syncSlider->setRange(cfg.thrusterMinPct(), cfg.thrusterMaxPct());
    g.syncSlider->setSingleStep(cfg.thrusterStepPct());
    g.syncSlider->setValue(0);
    g.syncSlider->setTickPosition(QSlider::TicksBelow);
    auto* syncCaption = new QLabel(QString::fromLocal8Bit("同步"), syncPage);
    if (orientation_ == Qt::Horizontal) {
        // 两行式：[同步 三态] / [水平滑条占满行宽]
        syncCaption->setFixedWidth(104);
        g.syncTarget->setFixedWidth(60);
        auto* cell = new QVBoxLayout();
        cell->setSpacing(2);
        auto* info = new QHBoxLayout();
        info->setSpacing(4);
        info->addWidget(syncCaption);
        info->addWidget(g.syncTarget, 1);
        cell->addLayout(info);
        cell->addWidget(g.syncSlider, 1);
        syncLay->addLayout(cell, 1);
    } else {
        syncCaption->setAlignment(Qt::AlignCenter);
        auto* column = new QVBoxLayout();
        column->setSpacing(2);
        column->addWidget(syncCaption, 0, Qt::AlignCenter);
        column->addWidget(g.syncTarget);
        column->addWidget(g.syncSlider, 1);
        syncLay->addLayout(column, 1);
        syncLay->addStretch(2);
    }
    g.stack->addWidget(syncPage);
    connect(g.syncSlider, &QSlider::valueChanged, this, [this, vertical](int v) {
        if (vertical) {
            vm_->setVerticalSyncTarget(v, false);
        } else {
            vm_->setHorizontalSyncTarget(v, false);
        }
    });
    connect(g.syncSlider, &QSlider::sliderReleased, this, [this, vertical] {
        const QSlider* s = (vertical ? verticalGroup_ : horizontalGroup_).syncSlider;
        if (vertical) {
            vm_->setVerticalSyncTarget(s->value(), true);
        } else {
            vm_->setHorizontalSyncTarget(s->value(), true);
        }
    });

    // 基准滑条页（姿态稳定 ON，BaseValueVH 双组独立基准；恒为水平滑条）
    auto* basePage = new QWidget(stackHost);
    auto* baseLay = new QHBoxLayout(basePage);
    baseLay->setSpacing(8);
    g.baseSlider = new QSlider(Qt::Horizontal, basePage);
    g.baseSlider->setRange(cfg.thrusterMinPct(), cfg.thrusterMaxPct());
    g.baseSlider->setValue(0);
    g.baseTarget = new QLabel(basePage);
    g.baseTarget->setFixedWidth(cfg.controlValueLabelWidth());
    auto* baseCaption = new QLabel(QString::fromLocal8Bit("基准"), basePage);
    if (orientation_ == Qt::Horizontal) {
        // 两行式：[基准 三态] / [水平滑条占满行宽]
        baseCaption->setFixedWidth(104);
        g.baseTarget->setFixedWidth(60);
        auto* cell = new QVBoxLayout();
        cell->setSpacing(2);
        auto* info = new QHBoxLayout();
        info->setSpacing(4);
        info->addWidget(baseCaption);
        info->addWidget(g.baseTarget, 1);
        cell->addLayout(info);
        cell->addWidget(g.baseSlider, 1);
        baseLay->addLayout(cell, 1);
    } else {
        baseLay->addWidget(baseCaption);
        baseLay->addWidget(g.baseSlider, 1);
        baseLay->addWidget(g.baseTarget);
    }
    g.stack->addWidget(basePage);
    connect(g.baseSlider, &QSlider::valueChanged, this, [this, vertical](int v) {
        if (vertical) {
            vm_->setVerticalBaseTarget(v, false);
        } else {
            vm_->setHorizontalBaseTarget(v, false);
        }
    });
    connect(g.baseSlider, &QSlider::sliderReleased, this, [this, vertical] {
        const QSlider* s = (vertical ? verticalGroup_ : horizontalGroup_).baseSlider;
        if (vertical) {
            vm_->setVerticalBaseTarget(s->value(), true);
        } else {
            vm_->setHorizontalBaseTarget(s->value(), true);
        }
    });

    vlay->addWidget(stackHost, 1);
    g.hint = new QLabel(g.group);
    g.hint->setWordWrap(true);
    g.hint->setStyleSheet(QStringLiteral("color:#888888; font-size:11px;"));
    vlay->addWidget(g.hint);
    return g.group;
}

// ---------------------------------------------------------------- 使能组

QGroupBox* ControlAreaWidget::buildEnableGroup()
{
    auto* group = new QGroupBox(QString::fromLocal8Bit("推进器使能"), this);
    // 主页：右列纵排；指令页：底部单行横排（不另占一列）
    QBoxLayout* lay = (orientation_ == Qt::Horizontal)
            ? static_cast<QBoxLayout*>(new QHBoxLayout(group))
            : static_cast<QBoxLayout*>(new QVBoxLayout(group));
    lay->setSpacing(12);
    enableAllSw_ = new SwitchButtonWidget(
            QString::fromLocal8Bit("推进器总使能"), group);
    enableVerticalSw_ = new SwitchButtonWidget(
            QString::fromLocal8Bit("垂直推进使能"), group);
    enableHorizontalSw_ = new SwitchButtonWidget(
            QString::fromLocal8Bit("水平推进使能"), group);
    lay->addWidget(enableAllSw_);
    lay->addWidget(enableVerticalSw_);
    lay->addWidget(enableHorizontalSw_);

    const auto bindEnable = [this](SwitchButtonWidget* w, SwitchId id) {
        connect(w, &SwitchButtonWidget::toggleRequested, this,
                [this, id, w](bool on) {
                    w->showPending(on); // 乐观显示，回退经刷新链修正
                    onEnableSwitchToggled(id, on);
                });
    };
    bindEnable(enableAllSw_, SwitchId::GlobalEnable);
    bindEnable(enableVerticalSw_, SwitchId::VerticalEnable);
    bindEnable(enableHorizontalSw_, SwitchId::HorizontalEnable);

    layoutHintLabel_ = new QLabel(group);
    layoutHintLabel_->setWordWrap(true);
    layoutHintLabel_->setStyleSheet(QStringLiteral("color:#888888; font-size:11px;"));
    if (orientation_ == Qt::Horizontal) {
        lay->addWidget(layoutHintLabel_, 1); // 横排：提示占满剩余宽度
    } else {
        lay->addWidget(layoutHintLabel_);
        lay->addStretch(1);
    }
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

// ---------------------------------------------------------------- 开关回调

void ControlAreaWidget::onSyncSwitchToggled(bool vertical, bool on)
{
    const SwitchId id = vertical ? SwitchId::VerticalSync : SwitchId::HorizontalSync;
    if (!safety_->switchToggleAllowed(id, on)) {
        refreshPermissions(); // 回到权威显示（Pending 禁重复点击等）
        return;
    }
    if (vertical) {
        vm_->requestVerticalSync(on);
    } else {
        vm_->requestHorizontalSync(on);
    }
}

void ControlAreaWidget::onEnableSwitchToggled(SwitchId id, bool on)
{
    if (!safety_->switchToggleAllowed(id, on)) {
        refreshPermissions();
        return;
    }
    switch (id) {
    case SwitchId::GlobalEnable:
        on ? vm_->requestMoveAll() : vm_->requestStopAll();
        break;
    case SwitchId::VerticalEnable:
        on ? vm_->requestMoveGroup(true) : vm_->requestStopGroup(true);
        break;
    case SwitchId::HorizontalEnable:
        on ? vm_->requestMoveGroup(false) : vm_->requestStopGroup(false);
        break;
    default:
        break; // 其余开关不在本组件
    }
}

// ---------------------------------------------------------------- 输入提交

bool ControlAreaWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        for (const RowUi& row : servoRows_) {
            if (row.input == watched) {
                return true;
            }
        }
        for (const GroupUi* g : {&verticalGroup_, &horizontalGroup_}) {
            for (const RowUi& row : g->rows) {
                if (row.input == watched) {
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ControlAreaWidget::commitServoInput(int uiNumber)
{
    const ChannelVm ch = vm_->servo(uiNumber);
    const QString text = servoRows_[uiNumber - 1].input->text().trimmed();
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || text.isEmpty()) {
        servoRows_[uiNumber - 1].input->setText(QString::number(ch.target));
        return;
    }
    vm_->setServoTarget(uiNumber, value, true);
    servoRows_[uiNumber - 1].input->setText(QString::number(vm_->servo(uiNumber).target));
}

void ControlAreaWidget::commitThrusterInput(bool vertical, int uiNumber)
{
    GroupUi& g = vertical ? verticalGroup_ : horizontalGroup_;
    const ChannelVm ch = vertical ? vm_->verticalThruster(uiNumber)
                                  : vm_->horizontalThruster(uiNumber);
    const QString text = g.rows[uiNumber - 1].input->text().trimmed();
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || text.isEmpty()) {
        g.rows[uiNumber - 1].input->setText(QString::number(ch.target));
        return;
    }
    if (vertical) {
        vm_->setVerticalThrusterTarget(uiNumber, value, true);
    } else {
        vm_->setHorizontalThrusterTarget(uiNumber, value, true);
    }
    g.rows[uiNumber - 1].input->setText(
            QString::number(vertical ? vm_->verticalThruster(uiNumber).target
                                     : vm_->horizontalThruster(uiNumber).target));
}

// ---------------------------------------------------------------- 三态刷新

void ControlAreaWidget::updateServoLabel(int uiNumber)
{
    const ChannelVm ch = vm_->servo(uiNumber);
    const QString confirmed = ch.confirmedValid
            ? QString::number(ch.confirmed) : QString::fromLocal8Bit("?");
    servoRows_[uiNumber - 1].target->setText(
            QString::fromLocal8Bit("%1°|%2").arg(ch.target).arg(confirmed));
    servoRows_[uiNumber - 1].slider->blockSignals(true);
    servoRows_[uiNumber - 1].slider->setValue(ch.target);
    servoRows_[uiNumber - 1].slider->blockSignals(false);
    if (!servoRows_[uiNumber - 1].input->hasFocus()) {
        servoRows_[uiNumber - 1].input->setText(QString::number(ch.target));
    }
}

void ControlAreaWidget::updateThrusterLabel(bool vertical, int uiNumber)
{
    GroupUi& g = vertical ? verticalGroup_ : horizontalGroup_;
    const ChannelVm ch = vertical ? vm_->verticalThruster(uiNumber)
                                  : vm_->horizontalThruster(uiNumber);
    const QString confirmed = ch.confirmedValid
            ? QString::number(ch.confirmed) : QString::fromLocal8Bit("?");
    g.rows[uiNumber - 1].target->setText(
            QString::fromLocal8Bit("%1|%2").arg(ch.target).arg(confirmed));
    g.rows[uiNumber - 1].slider->blockSignals(true);
    g.rows[uiNumber - 1].slider->setValue(ch.target);
    g.rows[uiNumber - 1].slider->blockSignals(false);
    if (!g.rows[uiNumber - 1].input->hasFocus()) {
        g.rows[uiNumber - 1].input->setText(QString::number(ch.target));
    }
}

void ControlAreaWidget::updateSyncUi(bool vertical)
{
    GroupUi& g = vertical ? verticalGroup_ : horizontalGroup_;
    const ChannelVm first = vertical ? vm_->verticalThruster(1)
                                     : vm_->horizontalThruster(1);
    const QString confirmed = first.confirmedValid
            ? QString::number(first.confirmed) : QString::fromLocal8Bit("?");
    g.syncTarget->setText(
            QString::fromLocal8Bit("%1|%2").arg(first.target).arg(confirmed));
    g.syncSlider->blockSignals(true);
    g.syncSlider->setValue(first.target);
    g.syncSlider->blockSignals(false);
}

void ControlAreaWidget::updateBaseUi(bool vertical)
{
    GroupUi& g = vertical ? verticalGroup_ : horizontalGroup_;
    const int target = vertical ? vm_->verticalBaseTarget()
                                : vm_->horizontalBaseTarget();
    const ChannelVm first = vertical ? vm_->verticalThruster(1)
                                     : vm_->horizontalThruster(1);
    const QString confirmed = first.confirmedValid
            ? QString::number(first.confirmed) : QString::fromLocal8Bit("?");
    g.baseTarget->setText(
            QString::fromLocal8Bit("%1%%2").arg(target).arg(confirmed));
    g.baseSlider->blockSignals(true);
    g.baseSlider->setValue(target);
    g.baseSlider->blockSignals(false);
}

// ---------------------------------------------------------------- 权限/布局

void ControlAreaWidget::refreshPermissions()
{
    // 舵机：仅断线禁用（与 Safe/姿态稳定/同步/Stop-Move/Estop/Emergency 解耦）
    servoGroup_->setEnabled(safety_->canServoIndividual());

    applyGroupUi(true);
    applyGroupUi(false);

    // 使能开关（权威四态绑定）
    enableAllSw_->bind(safety_, SwitchId::GlobalEnable);
    enableVerticalSw_->bind(safety_, SwitchId::VerticalEnable);
    enableHorizontalSw_->bind(safety_, SwitchId::HorizontalEnable);

    // 布局总提示
    const ModeState stab = safety_->switchState(SwitchId::AttitudeStab);
    if (stab == ModeState::On) {
        layoutHintLabel_->setText(QString::fromLocal8Bit(
                "姿态稳定模式：使用统一基准，Synchronization 状态将在退出"
                "姿态稳定后决定布局"));
    } else if (stab == ModeState::Unknown) {
        layoutHintLabel_->setText(QString::fromLocal8Bit(
                "姿态稳定状态未知：推进器保守禁用"));
    } else {
        layoutHintLabel_->setText(QString::fromLocal8Bit(
                "布局由各组 Synchronization 决定：同步开=单条，同步关=逐路"));
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

void ControlAreaWidget::applyGroupUi(bool vertical)
{
    GroupUi& g = vertical ? verticalGroup_ : horizontalGroup_;
    const ThrusterGroupLayout layout = vertical ? safety_->verticalLayout()
                                                : safety_->horizontalLayout();
    const bool operable = safety_->canThrusterGroup(vertical);

    g.stack->setCurrentIndex(layout == ThrusterGroupLayout::Individual ? 0
                             : layout == ThrusterGroupLayout::Sync ? 1 : 2);

    // 分组停止锁存/使能请求中：滑条与输入框全部置灰（§七.5）
    const bool indEnabled =
            operable && (layout == ThrusterGroupLayout::Individual);
    for (RowUi& row : g.rows) {
        row.slider->setEnabled(indEnabled);
        row.input->setEnabled(indEnabled);
    }
    g.syncSlider->setEnabled(operable && (layout == ThrusterGroupLayout::Sync));
    g.baseSlider->setEnabled(operable && (layout == ThrusterGroupLayout::Base));

    // 同步开关权威显示（姿态稳定 ON 时仍显示权威状态、可切换，布局不变）
    g.syncSwitch->bind(safety_, vertical ? SwitchId::VerticalSync
                                         : SwitchId::HorizontalSync);

    // 组提示
    if (safety_->switchState(SwitchId::AttitudeStab) == ModeState::On) {
        g.hint->setText(QString::fromLocal8Bit("统一基准控制（BaseValueVH）"));
    } else if (!operable) {
        g.hint->setText(vertical
                ? QString::fromLocal8Bit("垂直组不可操作（停止锁存/使能请求中/权威未知）")
                : QString::fromLocal8Bit("水平组不可操作（停止锁存/使能请求中/权威未知）"));
    } else if (layout == ThrusterGroupLayout::Sync) {
        g.hint->setText(QString::fromLocal8Bit("同步模式：单滑条控制全组"));
    } else {
        g.hint->setText(QString());
    }

    // 三态刷新
    const int count = vertical ? wire::kVerticalCount : wire::kHorizontalCount;
    for (int n = 1; n <= count; ++n) {
        updateThrusterLabel(vertical, n);
    }
    updateSyncUi(vertical);
    updateBaseUi(vertical);
}

} // namespace salacia
