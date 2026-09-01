#pragma once

#include <QObject>

#include "communication/FunctionRegistry.h"

namespace salacia {

// 模式四态：Unknown=未获权威值（断线/初始）；Pending=事务进行中（显示目标值）；
// On/Off=A35 权威
enum class ModeState
{
    Unknown,
    Pending,
    On,
    Off,
};

// 紧急按钮 UI 状态
enum class EmergencyButtonState
{
    Disabled,    // 断线：无法下发
    Ready,       // 可用
    Triggered,   // estop 已确认：显示已触发（仍可再次下发）
    InProgress,  // emergency 已确认：显示进行中
};

// 事务开关 ID（共享 PendingSwitchState 的 7 个开关）
enum class SwitchId
{
    Safe = 0,           // 安全保护模式（SafeOn/Off）
    AttitudeStab,       // 姿态稳定（Horizontal，Roll/Pitch 自动调平）
    GlobalEnable,       // 推进器总使能（MoveAll/StopAll）
    VerticalEnable,     // 垂直推进使能（MoveVertical/StopVertical）
    HorizontalEnable,   // 水平推进使能（MoveHorizontal/StopHorizontal）
    VerticalSync,       // 垂直同步（VerticalSyncOn/Off）
    HorizontalSync,     // 水平同步（HorizontalSyncOn/Off）
};

constexpr int kSwitchCount = 7;

// 推进器组布局（纯逻辑判定，UI 只做显隐不改判定）
enum class ThrusterGroupLayout
{
    Individual, // 姿态稳定 OFF 且同步 OFF：逐路滑条（同步 Unknown 亦保守逐路）
    Sync,       // 姿态稳定 OFF 且同步 ON：单条同步滑条
    Base,       // 姿态稳定 ON：单条基准滑条（BaseValueVH）
};

// 单个事务开关的完整状态：authoritative 与 Pending 显示目标分离，
// 单一 Pending 无法区分目标方向的红线即由此消除
struct PendingSwitchState
{
    ModeState authoritative = ModeState::Unknown; // 仅 Unknown/On/Off
    bool pending = false;                         // 事务进行中（禁重复点击）
    bool displayedTarget = false;                 // Pending 期间立即显示的目标
    quint16 pendingSeq = 0U;
    int pendingDirection = 0;                     // +1=ToOn / -1=ToOff
    ModeState rollbackState = ModeState::Unknown; // 发起时权威值（回退目标）
    qint64 pendingTimestampMs = 0;
    bool ackCommittedValid = false;               // 成功 ACK 已提交（冲突检测用）
    bool ackCommittedValue = false;
};

// 安全/模式状态机（主线程；集中式权限函数红线）
//
// 输入：TcpClient 连接状态、ACK/超时（请求生命周期）、StateEventV2（权威值）
// 输出：权限函数（canXxx 系列，UI 唯一可用性来源）+ stateChanged 通知
//
// 红线：
//  - 全部模式/使能/同步状态以 A35 ACK/StateEventV2 为权威；本地点击只产生
//    目标状态与 Pending，不当作成功；
//  - 失败 ACK/超时/断线 -> 回退发起前权威值并广播（UI 恢复显示 + 告警）；
//  - StateEventV2 先于 ACK 到达：以事件为权威并清除对应 Pending；
//  - 成功 ACK 后 StateEventV2 给出相反状态：事件覆盖 + 协议不一致高等级告警；
//  - 非法权威组合 Safe=ON 且 姿态稳定=OFF：锁定推进器 + 高等级告警，
//    Windows 端不悄悄修正；
//  - 舵机权限与 Safe/姿态稳定/同步/Stop-Move/Estop/Emergency 全部解耦，
//    仅要求 TCP 连接存在。
class SafetyStateModel : public QObject
{
    Q_OBJECT

public:
    explicit SafetyStateModel(QObject* parent = nullptr);

    // ---- 输入 ----
    void setConnected(bool on);
    // StateEventV2（0x0104）权威掩码（主链路）
    void applyAuthoritativeV2(quint16 stateMask);
    // legacy StateEvent（0x0102）兼容回退：仅含 safe/stab/estop/emergency 4 位，
    // 使能/同步开关保持原状态（无事件依据不臆造）
    void applyAuthoritative(quint8 legacyMask);
    // 请求生命周期（来自 TcpClient 信号链）
    void requestSent(quint16 seq, quint16 funcId);
    void requestAcked(quint16 seq, quint16 funcId, quint16 errCode);
    void requestFailed(quint16 seq, quint16 funcId); // 超时

    // ---- 权限函数（权限矩阵唯一权威，控件不得各自保存布尔）----
    bool canServoIndividual() const;            // 舵机：仅要求 TCP 已连接
    bool canThrusterGroup(bool vertical) const; // 分组推进器可操作
    bool canThrusterIndividual() const;         // 逐路推进器（姿态稳定 OFF 前提）
    bool canBaseSlider() const;                 // 基准滑条（姿态稳定 ON 前提）
    bool baseSliderVisible() const;             // 姿态稳定 ON -> 显示基准滑条
    bool thrustersLockedByAuthority() const { return illegalCombo_; }
    EmergencyButtonState estopButton() const;
    EmergencyButtonState emergencyButton() const;
    bool controlsLocked() const;                // 断线/权威未知整体提示用

    // ---- 布局判定（纯逻辑，可测）----
    ThrusterGroupLayout verticalLayout() const;
    ThrusterGroupLayout horizontalLayout() const;

    // ---- 开关状态读取（UI 显示）----
    ModeState switchState(SwitchId id) const;               // 含 Pending 四态
    bool switchPending(SwitchId id) const;
    bool switchDisplayedTarget(SwitchId id) const;          // pending ? 目标 : 权威
    bool switchToggleAllowed(SwitchId id, bool toOn) const; // Safe ON 期禁关姿态稳定
    ModeState safeState() const;            // 兼容旧 UI：= switchState(Safe)
    ModeState horizontalState() const;      // 兼容旧 UI：= switchState(AttitudeStab)
    bool estopActive() const { return estopOn_; }
    bool emergencyActive() const { return emergencyOn_; }
    bool authorityKnown() const { return authorityKnown_; }
    bool connected() const { return connected_; }

signals:
    void stateChanged();                         // 任意权限相关状态变化（UI 拉取）
    void modeRejected(quint16 funcId, quint16 errCode); // NACK 回退（UI 恢复+提示）
    void switchTimeout(quint16 funcId);          // 超时回退
    void authorityConflict(const QString& detail); // 协议不一致/非法权威组合（高等级）

private:
    static bool switchFuncTarget(quint16 funcId, SwitchId& out, bool& toOn);
    void beginPending(SwitchId id, bool toOn, quint16 seq);
    bool commitPendingBySeq(quint16 seq, bool success); // 返回是否有事务被消费
    void applySwitchAuthority(SwitchId id, bool on);
    void recheckIllegalCombo();

    bool connected_ = false;
    bool authorityKnown_ = false;
    PendingSwitchState switches_[kSwitchCount];
    bool estopOn_ = false;
    bool emergencyOn_ = false;
    bool illegalCombo_ = false;  // Safe=ON 且 姿态稳定=OFF 的非法权威组合
    quint16 lastMaskV2_ = 0U;    // 最近 V2 权威掩码
};

} // namespace salacia
