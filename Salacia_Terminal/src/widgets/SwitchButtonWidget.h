#pragma once

#include <QWidget>

#include "core/SafetyStateModel.h"

class QCheckBox;
class QLabel;
class QProgressBar;

namespace salacia {

// 事务开关行（共享组件）：标题 + SwitchButton + ProgressRing + 状态文字
//
// 四态绑定（bind）：On=开启 / Off=关闭 / Pending=目标态+Ring+禁点 /
// Unknown=半选态+禁用+"状态未知"（不得显示为已关闭）；
// Safe ON 期间姿态稳定锁定并显示说明（单向联动红线）。
// 用户点击 -> toggleRequested(目标态)；权限判定与发送在调用方。
class SwitchButtonWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SwitchButtonWidget(const QString& title, QWidget* parent = nullptr);

    // 按 SafetyStateModel 权威状态绑定显示（含 Safe 联动锁定特例）
    void bind(SafetyStateModel* model, SwitchId id);
    // 乐观显示：用户点击后立即进入目标态 + Ring（等待 Pending/回退/告警修正）
    void showPending(bool targetOn);
    // 锁定显示：保持当前值 + 禁用 + 原因说明
    void showLocked(bool currentOn, const QString& reason);
    bool isOn() const;

signals:
    void toggleRequested(bool targetOn); // 用户点击（目标态）

private:
    void applyDisplay(Qt::CheckState checkState, bool ringVisible, bool enabled,
                      const QString& status);

    QLabel* title_ = nullptr;
    QCheckBox* sw_ = nullptr;
    QProgressBar* ring_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace salacia
