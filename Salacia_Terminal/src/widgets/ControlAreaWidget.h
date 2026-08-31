#pragma once

#include <QWidget>

#include <QVector>

class QGroupBox;
class QLabel;
class QLineEdit;
class QSlider;
class QPushButton;

namespace salacia {

class ControlViewModel;
class SafetyStateModel;

// 控制区共享组件（主页与指令页复用）
//
// 内容：舵机组（N 路竖直滑条 + 上方三态标签 + 下方输入框）+
//       推进器组（M 路同构；horizontal on 时整组隐藏改由基准组接管）+
//       基准组（horizontal on 专用单滑条）+ 可选紧急固定区。
class ControlAreaWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ControlAreaWidget(ControlViewModel* viewModel,
                               SafetyStateModel* safety,
                               bool withEmergencyArea,
                               QWidget* parent = nullptr);

    void refreshPermissions(); // SafetyStateModel::stateChanged -> 此处

protected:
    bool eventFilter(QObject* watched, QEvent* event) override; // 输入框滚轮拦截

private:
    QGroupBox* buildServoGroup();
    QGroupBox* buildThrusterGroup();
    QGroupBox* buildBaseGroup();
    QWidget* buildEmergencyArea();
    void commitServoInput(int id);
    void commitThrusterInput(int id);
    void updateServoLabel(int id);
    void updateThrusterLabel(int id);

    ControlViewModel* vm_ = nullptr;
    SafetyStateModel* safety_ = nullptr;

    struct RowUi
    {
        QSlider* slider = nullptr;
        QLabel* target = nullptr;
        QLineEdit* input = nullptr;
    };
    QVector<RowUi> servoRows_;
    QVector<RowUi> thrusterRows_;

    QGroupBox* servoGroup_ = nullptr;
    QGroupBox* thrusterGroup_ = nullptr;
    QGroupBox* baseGroup_ = nullptr;
    QSlider* baseSlider_ = nullptr;
    QLabel* baseValue_ = nullptr;

    QPushButton* estopBtn_ = nullptr;
    QPushButton* emergencyBtn_ = nullptr;
    QLabel* modeHintLabel_ = nullptr;
};

} // namespace salacia
