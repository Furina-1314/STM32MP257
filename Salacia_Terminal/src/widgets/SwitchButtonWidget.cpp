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

void SwitchButtonWidget::bind(SafetyStateModel* model, SwitchId id)
{
    const ModeState state = model->switchState(id);
    const bool target = model->switchDisplayedTarget(id);
    // Safe 单向联动红线：Safe ON 期间姿态稳定保持 ON + 禁用 + 说明
    if ((id == SwitchId::AttitudeStab) && (state == ModeState::On)
        && (model->switchState(SwitchId::Safe) == ModeState::On)) {
        showLocked(true, QString::fromLocal8Bit("Safe 模式要求姿态稳定开启"));
        return;
    }
    switch (state) {
    case ModeState::On:
        applyDisplay(Qt::Checked, false, true, QString::fromLocal8Bit("已开启"));
        break;
    case ModeState::Off:
        applyDisplay(Qt::Unchecked, false, true, QString::fromLocal8Bit("已关闭"));
        break;
    case ModeState::Pending:
        applyDisplay(target ? Qt::Checked : Qt::Unchecked, true, false,
                     QString::fromLocal8Bit("请求中..."));
        break;
    case ModeState::Unknown:
    default:
        // Unknown 不得显示为已关闭：半选态 + 禁用 + 状态未知
        applyDisplay(Qt::PartiallyChecked, false, false,
                     QString::fromLocal8Bit("状态未知"));
        break;
    }
}

} // namespace salacia
