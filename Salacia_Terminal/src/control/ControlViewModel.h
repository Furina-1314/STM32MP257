#pragma once

#include <QObject>

#include <QMap>
#include <QTimer>
#include <QVector>

#include "communication/FunctionRegistry.h"
#include "core/SafetyStateModel.h"

namespace salacia {

// 单通道三态（目标值=UI 输入；已发送=最近出队值；确认值=A35 ACK 回报）
struct ChannelVm
{
    int target = 0;
    int sent = 0;
    int confirmed = 0;
    bool confirmedValid = false; // 尚无 ACK 时显示"未知"而非 0 冒充
};

// 控制 ViewModel（主线程；UI 只绑此模型，不直接拼报文）
//
// 职责：
//  - 10 舵机 + 6 推进器通道三态管理（数量/值域/节拍全部来自 [control]）；
//  - 滑条限频合并（slider_rate_limit_ms）+ 松手立即冲刷（release_flush）；
//  - horizontal on 切换：清空逐路待发（隐藏控件不得稍后发送旧值红线），
//    基准值走单帧 BaseValue；
//  - estop/emergency 一键直发（绕过合并节拍；确认开关来自 [ui]）；
//  - 权限门控：全部发送入口先过 SafetyStateModel 权限函数。
//
// 发送出口（MainWindow 接线到 TcpClient）：
//  - sendRequested(funcId, payload)   普通逐路/基准
//  - estopRequested(payload) / emergencyRequested() 紧急直发
class ControlViewModel : public QObject
{
    Q_OBJECT

public:
    explicit ControlViewModel(SafetyStateModel* safety, QObject* parent = nullptr);

    // ---- 通道访问 ----
    int servoCount() const { return static_cast<int>(servos_.size()); }
    int thrusterCount() const { return static_cast<int>(thrusters_.size()); }
    ChannelVm servo(int id) const;        // id: 1..N
    ChannelVm thruster(int id) const;
    int baseTarget() const { return baseTarget_; }

    // ---- UI 写入口（权限不足返回 false 并发 permissionBlocked）----
    bool setServoTarget(int id, int deg, bool released);
    bool setThrusterTarget(int id, int pct, bool released);
    bool setBaseTarget(int pct, bool released);
    void servoMid(int id);                // 单路归中（90°）
    void allThrustersNeutral();           // 全部中位（= 逐路 0 正常链路）
    void requestEstop();                  // estop 单帧直发（确认门控在 UI 层）
    void requestEmergency();

    // ---- ACK/超时回填（经 MainWindow 从 TcpClient 转发）----
    void onFrameSent(quint16 seq, quint16 funcId, const QByteArray& payload);
    void onFrameAcked(quint16 seq, quint16 errCode);
    void onFrameFailed(quint16 seq);

    // ---- 模式切换（SafetyStateModel 驱动）----
    void onHorizontalChanged(bool on);

    bool estopConfirmRequired() const { return estopConfirm_; }
    bool emergencyConfirmRequired() const { return emergencyConfirm_; }

signals:
    void sendRequested(quint16 funcId, const QByteArray& payload);
    void estopRequested(const QByteArray& payload);
    void emergencyRequested();
    void channelUpdated(int channelKind, int id); // 0=舵机 1=推进器（UI 拉取三态）
    void baseUpdated();
    void permissionBlocked(const QString& reason);
    void channelUnknown(int channelKind, int id); // 超时：显示状态未知（不自动重发）

private slots:
    void flushPending();

private:
    void sendServoNow(int id);
    void sendThrusterNow(int id);
    void sendBaseNow();
    bool checkServoAllowed();
    bool checkThrusterAllowed();
    bool checkBaseAllowed();

    SafetyStateModel* safety_ = nullptr;

    QVector<ChannelVm> servos_;
    QVector<ChannelVm> thrusters_;
    int baseTarget_ = 0;

    QTimer flushTimer_;
    QMap<int, int> pendingServo_;    // id -> deg（限频合并）
    QMap<int, int> pendingThruster_; // id -> pct
    bool basePending_ = false;

    struct SentTrack
    {
        int kind = 0; // 0=舵机 1=推进器 2=基准
        int id = 0;
        int value = 0;
    };
    QMap<quint16, SentTrack> inFlight_; // seq -> 通道回填

    // 配置快照
    int servoMinDeg_ = 0;
    int servoMaxDeg_ = 180;
    int servoMidDeg_ = 90;
    int thrusterMinPct_ = -100;
    int thrusterMaxPct_ = 100;
    bool estopConfirm_ = false;
    bool emergencyConfirm_ = true;
    bool releaseFlush_ = true;
};

} // namespace salacia
