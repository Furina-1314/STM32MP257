#include "SwitchButtonWidget.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>

namespace salacia {

SwitchButtonWidget::SwitchButtonWidget(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    title_ = new QLabel(title, this);
    title_->setFixedWidth(190);
    sw_ = new QCheckBox(this);
    sw_->setProperty("isSwitchButton", true); // FluentUI SwitchButton 样式
    sw_->setTristate(true);                   // Unknown 以半选态表达
    ring_ = new QProgressBar(this);
    ring_->setRange(0, 0); // 不确定进度（请求中旋转环）
    ring_->setTextVisible(false);
    ring_->setFixedSize(22, 22);
    ring_->setVisible(false);
    status_ = new QLabel(this);
    status_->setStyleSheet(QStringLiteral("color:#888888; font-size:11px;"));
    layout->addWidget(title_);
    layout->addWidget(sw_);
    layout->addWidget(ring_);
    layout->addWidget(status_, 1);

    // clicked 仅由真实用户交互触发（编程式 setCheckState 不发）
    connect(sw_, &QCheckBox::clicked, this, [this](bool checked) {
        emit toggleRequested(checked);
    });
}

bool SwitchButtonWidget::isOn() const
{
    return sw_->checkState() == Qt::Checked;
}

void SwitchButtonWidget::applyDisplay(Qt::CheckState checkState, bool ringVisible,
                                      bool enabled, const QString& status)
{
    const QSignalBlocker block(sw_); // 编程式置态不得误触发
    sw_->setCheckState(checkState);
    sw_->setEnabled(enabled);
    ring_->setVisible(ringVisible);
    status_->setText(status);
}

void SwitchButtonWidget::showPending(bool targetOn)
{
    applyDisplay(targetOn ? Qt::Checked : Qt::Unchecked, true, false,
                 QString::fromLocal8Bit("请求中..."));
}

void SwitchButtonWidget::showLocked(bool currentOn, const QString& reason)
{
    applyDisplay(currentOn ? Qt::Checked : Qt::Unchecked, false, false, reason);
}

namespace {

// 四态 -> 勾选态（Unknown 半选：不得显示为已关闭）
Qt::CheckState checkStateOf(ModeState state, bool target)
{
    switch (state) {
    case ModeState::On:
        return Qt::Checked;
    case ModeState::Off:
        return Qt::Unchecked;
    case ModeState::Pending:
        return target ? Qt::Checked : Qt::Unchecked;
    case ModeState::Unknown:
    default:
        return Qt::PartiallyChecked;
    }
}

QString stateText(ModeState state)
{
    switch (state) {
    case ModeState::On:
        return QString::fromLocal8Bit("已开启");
    case ModeState::Off:
        return QString::fromLocal8Bit("已关闭");
    case ModeState::Pending:
        return QString::fromLocal8Bit("请求中...");
    case ModeState::Unknown:
    default:
        return QString::fromLocal8Bit("状态未知");
    }
}

} // namespace

void SwitchButtonWidget::bind(SafetyStateModel* model, SwitchId id)
{
    const ModeState state = model->switchState(id);
    const bool target = model->switchDisplayedTarget(id);

    // Safe 单向联动红线：Safe ON 期间姿态稳定保持 ON + 禁用 + 说明；
    // Safe ON 且姿态稳定 OFF 为非法权威组合（模型已锁推进器并高等级告警）
    if ((id == SwitchId::AttitudeStab)
        && (model->switchState(SwitchId::Safe) == ModeState::On)) {
        if (state == ModeState::On) {
            showLocked(true, QString::fromLocal8Bit("Safe 模式要求姿态稳定开启"));
            return;
        }
        if (state == ModeState::Off) {
            showLocked(false, QString::fromLocal8Bit(
                    "非法权威状态（Safe=ON 且姿态稳定=OFF），推进器已锁定"));
            return;
        }
        // Pending：SafeOn 联动事务进行中，走通用四态
    }

    // 总使能联动：总使能非 ON（OFF/Pending/Unknown）时垂直/水平使能开关置灰，
    // 保留权威勾选显示并说明原因（总使能 OFF 期间禁止发起分组 Move）
    if (((id == SwitchId::VerticalEnable) || (id == SwitchId::HorizontalEnable))
        && (model->switchState(SwitchId::GlobalEnable) != ModeState::On)) {
        applyDisplay(checkStateOf(state, target), state == ModeState::Pending,
                     false,
                     stateText(state) + QString::fromLocal8Bit("（总使能非 ON）"));
        return;
    }

    applyDisplay(checkStateOf(state, target), state == ModeState::Pending,
                 (state == ModeState::On) || (state == ModeState::Off),
                 stateText(state));
}

} // namespace salacia
