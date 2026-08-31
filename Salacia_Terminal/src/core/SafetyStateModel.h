#pragma once

#include <QObject>
#include <QMap>

#include "communication/FunctionRegistry.h"

namespace salacia {

// 模式三态：Unknown=未获权威值（断线/初始）；Pending=请求中；On/Off=权威
enum class ModeState
{
    Unknown,
    Pending,
    On,
    Off,
};

// 紧急按钮 UI 状态（对应权限矩阵第五/六列的"可用/已触发/进行中/无法下发"）
enum class EmergencyButtonState
{
    Disabled,    // 断线或状态未知：显示无法下发
    Ready,       // 可用
    Triggered,   // estop 已确认：显示已触发（仍可再次下发）
    InProgress,  // emergency 已确认：显示进行中
};

// 安全/模式状态机（主线程；集中式权限函数红线）
//
// 输入：TcpClient 连接状态、ACK/超时（请求生命周期）、StateEvent（权威值）
// 输出：权限函数（canXxx 系列，UI 唯一可用性来源）+ stateChanged 通知
//
// 红线：
//  - 模式以 A35 ACK/状态回报为准；本地点击只置 Pending，不当作成功；
//  - 请求失败/超时 -> 回滚到上一权威值并广播（UI 恢复显示）；
//  - 权威状态未知（断线/首连未收 StateEvent）时全部普通控制禁用。
class SafetyStateModel : public QObject
{
    Q_OBJECT

public:
    explicit SafetyStateModel(QObject* parent = nullptr);

    // ---- 输入 ----
    void setConnected(bool on);
    // A35 主动状态事件 / status 响应解析出的权威掩码
    void applyAuthoritative(quint8 stateMask);
    // 请求生命周期（来自 TcpClient 信号链）
    void requestSent(quint16 seq, quint16 funcId);
    void requestAcked(quint16 seq, quint16 funcId, quint16 errCode);
    void requestFailed(quint16 seq, quint16 funcId); // 超时/错误

    // ---- 权限函数（权限矩阵唯一权威，控件不得各自保存布尔）----
    bool canServoIndividual() const;      // 舵机逐路
    bool canThrusterIndividual() const;   // 推进器逐路（horizontal on 时隐藏）
    bool canBaseSlider() const;           // 统一基准滑条（仅 horizontal on）
    bool baseSliderVisible() const;       // horizontal on -> 显示基准滑条
    EmergencyButtonState estopButton() const;
    EmergencyButtonState emergencyButton() const;
    bool controlsLocked() const;          // 全部普通控制禁用（UI 提示用）

    // ---- 状态读取（UI 显示）----
    ModeState safeState() const { return safe_; }
    ModeState horizontalState() const { return horizontal_; }
    bool estopActive() const { return estopOn_; }
    bool emergencyActive() const { return emergencyOn_; }
    bool authorityKnown() const { return authorityKnown_; }
    bool connected() const { return connected_; }

signals:
    void stateChanged();            // 任意权限相关状态变化（无载荷，UI 拉取）
    void modeRejected(quint16 funcId, quint16 errCode); // 请求被拒（UI 恢复+提示）

private:
    struct PendingRequest
    {
        quint16 funcId = 0U;
        qint64 atMs = 0;
    };

    static bool modeFunc(quint16 funcId); // 该 funcId 是否模式类命令
    void applyModeResult(quint16 funcId, bool success);

    bool connected_ = false;
    bool authorityKnown_ = false;
    ModeState safe_ = ModeState::Unknown;
    ModeState horizontal_ = ModeState::Unknown;
    bool estopOn_ = false;
    bool emergencyOn_ = false;
    quint8 lastMask_ = 0U; // 最近权威掩码（请求失败回滚用）

    QMap<quint16, PendingRequest> pending_; // seq -> 请求
};

} // namespace salacia
