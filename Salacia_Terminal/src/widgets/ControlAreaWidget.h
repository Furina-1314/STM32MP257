#pragma once

#include <QWidget>

#include <QVector>

#include "core/SafetyStateModel.h"

class QGroupBox;
class QLabel;
class QLineEdit;
class QSlider;
class QStackedLayout;
class QPushButton;

namespace salacia {

class ControlViewModel;
class SafetyStateModel;
class SwitchButtonWidget;

// 控制区共享组件（主页与指令页复用）
//
// 内容：
//  - 舵机组：10 路竖直滑条（舵机1（CH0）..舵机10（CH9）；三态标签+输入框），
//    仅断线禁用（与全部推进器模式解耦红线）；
//  - 垂直推进器组（CH10-13）/ 水平推进器组（CH14-15）：各带 Synchronization
//    开关；组内布局按权威状态动态切换（独立滑条 / 同步滑条 / 基准滑条）；
//  - 推进器使能列：总使能/垂直/水平三级开关 + 布局提示；
//  - 可选紧急固定区（estop/emergency + 模式提示）。
//
// 布局与权限判定只读 SafetyStateModel（唯一权威），本组件不做业务判断；
// 所有滑条保留 目标值/已发送值/A35 确认值 三态显示。
class ControlAreaWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ControlAreaWidget(ControlViewModel* viewModel,
                               SafetyStateModel* safety,
                               bool withEmergencyArea,
                               Qt::Orientation sliderOrientation = Qt::Vertical,
                               QWidget* parent = nullptr);

    void refreshPermissions(); // SafetyStateModel::stateChanged -> 此处

protected:
    bool eventFilter(QObject* watched, QEvent* event) override; // 输入框滚轮拦截

private:
    struct RowUi
    {
        QSlider* slider = nullptr;
        QLabel* target = nullptr;
        QLineEdit* input = nullptr;
    };
    struct GroupUi
    {
        QGroupBox* group = nullptr;
        QStackedLayout* stack = nullptr; // 0=独立 1=同步 2=基准
        QVector<RowUi> rows;             // 独立滑条（组内编号 1..N）
        QSlider* syncSlider = nullptr;   // 同步滑条（组同步 ON）
        QLabel* syncTarget = nullptr;
        QSlider* baseSlider = nullptr;   // 基准滑条（姿态稳定 ON）
        QLabel* baseTarget = nullptr;
        SwitchButtonWidget* syncSwitch = nullptr;
        QLabel* hint = nullptr;          // 组内布局/锁存提示
    };

    QGroupBox* buildServoGroup();
    QGroupBox* buildThrusterGroup(bool vertical);
    QGroupBox* buildEnableGroup();
    QWidget* buildEmergencyArea();
    void onSyncSwitchToggled(bool vertical, bool on);
    void onEnableSwitchToggled(SwitchId id, bool on);
    void commitServoInput(int uiNumber);
    void commitThrusterInput(bool vertical, int uiNumber);
    void updateServoLabel(int uiNumber);
    void updateThrusterLabel(bool vertical, int uiNumber);
    void updateSyncUi(bool vertical);
    void updateBaseUi(bool vertical);
    void applyGroupUi(bool vertical);

    ControlViewModel* vm_ = nullptr;
    SafetyStateModel* safety_ = nullptr;
    Qt::Orientation orientation_ = Qt::Vertical; // 滑条方向（主页竖直/指令页水平）

    QVector<RowUi> servoRows_;
    QGroupBox* servoGroup_ = nullptr;

    GroupUi verticalGroup_;
    GroupUi horizontalGroup_;

    SwitchButtonWidget* enableAllSw_ = nullptr;
    SwitchButtonWidget* enableVerticalSw_ = nullptr;
    SwitchButtonWidget* enableHorizontalSw_ = nullptr;
    QLabel* layoutHintLabel_ = nullptr;

    QPushButton* estopBtn_ = nullptr;
    QPushButton* emergencyBtn_ = nullptr;
    QLabel* modeHintLabel_ = nullptr;
};

} // namespace salacia
