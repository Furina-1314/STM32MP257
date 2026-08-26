#pragma once

#include <QWidget>

#include <QMap>
#include <QTimer>

class QGroupBox;
class QLabel;
class QPushButton;
class QSlider;

namespace salacia {

// 执行机构遥控面板（16 路 PWM 独立控制）
//
// 板端 CLI 约定（板侧按此实现，详见 .vibestate.md）：
//   pwm <id> <us>    id: 1-10 = 舵机（角度），11-16 = 推进器
//   应答文本任意，退出码 0 为成功
//
// UI 结构：
//   舵机组（通道 1-10）：角度滑条 0~180° -> [servo_min_us, servo_max_us]
//   推进器组（通道 11-16）：油门滑条 -100~+100% ->
//       neutral + pct*(max-min)/200，配"中位"快捷键
//   紧急停机：全部推进器立即中位 + 舵机回中（90°）
//
// 下发策略（指令 ≤50ms 红线）：
//   拖动仅更新界面与待发表；50ms 合并节拍将每个通道的最新值以
//   pwmCommandRequested 发出（陈旧值被覆盖，控制语义取新鲜度）。
//   SSH 离线时指令进入 SshClient 队列，连接建立瞬间队列被清空
//   （防止重连后执行过期动作）。
class ControlPanelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ControlPanelWidget(QWidget* parent = nullptr);

    // SSH 链路状态（仅影响提示文字，不锁操作：离线指令入队且连接时清空）
    void setLinkStatus(bool connected);

signals:
    // 通道 id（1-16）与目标脉冲宽度（微秒）
    void pwmCommandRequested(int deviceId, int pulseUs);
    // 紧急停机已在本地面板执行（通知层用于记录/声光提示）
    void emergencyStopRequested();

private slots:
    void flushPending(); // 50ms 合并节拍：每通道最新值下发

private:
    QGroupBox* buildServoGroup();
    QGroupBox* buildThrusterGroup();
    QWidget* buildFooter();

    void queueServo(int channel);    // channel 1..10
    void queueThruster(int channel); // channel 1..6
    int servoUs(int channel) const;
    int thrusterUs(int channel) const;
    void updateServoLabel(int channel);
    void updateThrusterLabel(int channel);
    void emergencyStop();

    struct RowUi
    {
        QSlider* slider = nullptr;
        QLabel* value = nullptr;
    };
    RowUi servoRows_[10];
    RowUi thrusterRows_[6];

    QTimer flushTimer_;
    QMap<int, int> pending_; // 通道 id -> 待发 us

    // PWM 量程（app_config.ini [control]，参数解耦红线）
    int servoMinUs_ = 500;
    int servoMaxUs_ = 2500;
    int thrusterMinUs_ = 1100;
    int thrusterMaxUs_ = 1900;
    int thrusterNeutralUs_ = 1500;

    QLabel* linkHint_ = nullptr;
};

} // namespace salacia
