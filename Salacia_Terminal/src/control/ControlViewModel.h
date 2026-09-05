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
//  - 10 舵机 + 垂直 4（CH10-13）/ 水平 2（CH14-15）推进器通道三态管理；
//    拓扑常量唯一来源为 WireConstants，UI 编号与 wireId 一律经映射助手换算，
//    禁止 id-1 之类隐式兼任协议 ID；
//  - 滑条限频合并（slider_rate_limit_ms）+ 松手立即冲刷（release_flush）；
//  - 姿态稳定 ON：清空逐路待发（隐藏控件不得稍后发送旧值红线），
//    基准走 BaseValueVH 单帧双值（垂直基准、水平基准）；
//  - Stop/Move 三级使能请求（总/垂直/水平）；分组停止锁存期间丢弃该组待发，
//    重新使能不自动恢复或重发旧目标（不重放红线）；
//  - estop/emergency 一键直发（空载荷，绕过合并节拍）；
//  - 权限门控：全部发送入口先过 SafetyStateModel 权限函数。
//
// 发送出口（MainWindow 接线到 TcpClient）：
//  - sendRequested(funcId, payload)   普通逐路/基准/Stop-Move
//  - estopRequested(payload)/emergencyRequested() 紧急直发（载荷为空）
class ControlViewModel : public QObject
{
    Q_OBJECT

public:
    explicit ControlViewModel(SafetyStateModel* safety, QObject* parent = nullptr);

    // ---- 通道访问 ----
    int servoCount() const { return static_cast<int>(servos_.size()); }       // 10
    int verticalCount() const { return wire::kVerticalCount; }                // 4
    int horizontalCount() const { return wire::kHorizontalCount; }            // 2
    int thrusterCount() const { return verticalCount() + horizontalCount(); } // 6（兼容）
    ChannelVm servo(int uiNumber) const;               // UI 1..10（wire 0..9）
    ChannelVm verticalThruster(int uiNumber) const;    // 组内 1..4（wire 10..13）
    ChannelVm horizontalThruster(int uiNumber) const;  // 组内 1..2（wire 14..15）
    ChannelVm thruster(int flat) const;                // 兼容桥接：扁平 1..6
    int verticalBaseTarget() const { return verticalBaseTarget_; }
    int horizontalBaseTarget() const { return horizontalBaseTarget_; }
    int baseTarget() const { return verticalBaseTarget_; } // 兼容：旧单基准滑条

    // ---- UI 写入口（权限不足返回 false 并发 permissionBlocked）----
    bool setServoTarget(int uiNumber, int deg, bool released);
    bool setVerticalThrusterTarget(int uiNumber, int pct, bool released);
    bool setHorizontalThrusterTarget(int uiNumber, int pct, bool released);
    bool setThrusterTarget(int flat, int pct, bool released); // 兼容桥接
    void setVerticalSyncTarget(int pct, bool released);       // 同步滑条：全组同值
    void setHorizontalSyncTarget(int pct, bool released);
    bool setVerticalBaseTarget(int pct, bool released);       // 姿态稳定基准
    bool setHorizontalBaseTarget(int pct, bool released);
    bool setBaseTarget(int pct, bool released);               // 兼容：双组同值
    void servoMid(int uiNumber);                              // 单路归中（90°）
    // Stop/Move 三级使能（ON=Move 允许运动 / OFF=Stop 停止锁存）
    void requestMoveAll();
    void requestStopAll();
    void requestMoveGroup(bool vertical);
    void requestStopGroup(bool vertical);
    // 模式开关（Safe/姿态稳定/双同步）；事务 Pending 由 SafetyStateModel
    // 经 requestSent 链管理，此处只做链路门控与发送
    void requestSafeMode(bool on);
    void requestAttitudeStab(bool on);
    void requestVerticalSync(bool on);
    void requestHorizontalSync(bool on);
    void requestEstop();                  // 空载荷直发（确认门控在 UI 层）
    void requestEmergency();

    // ---- ACK/超时回填（经 MainWindow 从 TcpClient 转发）----
    void onFrameSent(quint16 seq, quint16 funcId, const QByteArray& payload);
    void onFrameAcked(quint16 seq, quint16 errCode);
    void onFrameFailed(quint16 seq);

    // ---- 权威状态变化（SafetyStateModel::stateChanged 驱动，全量同步）----
    void onAuthorityStateChanged();

    bool estopConfirmRequired() const { return estopConfirm_; }
    bool emergencyConfirmRequired() const { return emergencyConfirm_; }

signals:
    void sendRequested(quint16 funcId, const QByteArray& payload);
    void estopRequested(const QByteArray& payload); // 载荷为空（仅推进器置零）
    void emergencyRequested();
    void channelUpdated(int channelKind, int id); // 0=舵机 1=推进器（扁平 1..6）
    void baseUpdated();
    void permissionBlocked(const QString& reason);
    void channelUnknown(int channelKind, int id);  // 超时：显示状态未知（不自动重发）

private slots:
    void flushPending();

private:
    void sendServoNow(int uiNumber);
    void sendThrusterNow(int flat);
    void sendBaseNow();
    bool checkServoAllowed();
    bool checkThrusterGroupAllowed(bool vertical);
    bool checkBaseAllowed();
    void clearGroupPending(bool vertical);

    SafetyStateModel* safety_ = nullptr;

    QVector<ChannelVm> servos_;    // 10；索引 = UI 编号-1（wire 0..9）
    QVector<ChannelVm> thrusters_; // 6；扁平 1..4=垂直 5..6=水平（wire 10..15）
    int verticalBaseTarget_ = 0;
    int horizontalBaseTarget_ = 0;

    QTimer flushTimer_;
    QMap<int, int> pendingServo_;     // UI 编号 -> deg（限频合并）
    QMap<int, int> pendingThruster_;  // 扁平 id -> pct（限频合并）
    bool pendingVerticalBase_ = false;
    bool pendingHorizontalBase_ = false;

    struct SentTrack
    {
        int kind = 0;  // 0=舵机 1=推进器 2=基准（双值）
        int id = 0;    // 舵机 UI 编号 / 推进器扁平 id
        int value = 0; // 舵机角度 / 推进器百分比
        int valueV = 0; // 基准：垂直
        int valueH = 0; // 基准：水平
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
