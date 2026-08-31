#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>

#include <vector>

#include "WireConstants.h"

// ============================================================================
// Windows 侧通信函数注册表（唯一 Function ID/命令名权威来源）
//
// 表项只描述 Windows 的编码/发送/接收/UI 分发元数据；不包含任何板端
// 处理逻辑。typed 载荷编解码函数与表同文件族（FunctionRegistry.cpp），
// 调用方（TcpClient/指令页/控制 ViewModel）一律经查表 + 强类型函数，
// 禁止在控件或网络类中散落 funcId 与命令字符串。
// ============================================================================
namespace salacia::wire {

enum class Direction
{
    Request,   // Windows -> A35
    Response,  // A35 -> Windows（被动）
    Event,     // A35 -> Windows（主动）
    Telemetry, // A35 -> Windows（周期流）
};

enum class Category
{
    System,
    Safety,
    Mode,
    Servo,
    Propeller,
    Sensor,
    Link,
};

struct FunctionEntry
{
    quint16 funcId;
    const char* name;      // 人类可读命令名（对齐协议文档语义）
    Direction direction;
    Category category;
    bool needsAck;
    int priority;          // kPriority*（小者优先）
    const char* vmUpdate;  // 关联 ViewModel 状态更新（Phase 3 接线）
    const char* degrade;   // 对端能力缺失时的 Windows 降级行为说明
};

class FunctionRegistry
{
public:
    static const std::vector<FunctionEntry>& all();
    static const FunctionEntry* findByFuncId(quint16 funcId);
    static const FunctionEntry* findByName(const QString& name);
    static int count();
};

// ---------------------------------------------------------------- 强类型载荷

struct ServoSetCmd            // set servo <id> <angle>
{
    quint8 id = 0U;           // 0 = all
    quint16 angleDeg = 90U;   // 0-180
};

struct PropellerSetCmd        // set propeller <id> <value>
{
    quint8 id = 0U;           // 0 = all
    qint16 valuePct = 0;      // -100..+100
};

struct SensorSummary          // 100Hz 汇总（A35 -> Windows）
{
    float tempC = 0.0F;
    float humidPct = 0.0F;
    float accelMps2[3] = {0.0F, 0.0F, 0.0F};
    float gyroRadS[3] = {0.0F, 0.0F, 0.0F};
    float voltage = 0.0F;
    float distMm = kInvalidDistanceMm;
    quint8 validMask = 0U;
    quint32 boardTimeMs = 0U;
};

struct AckResult
{
    quint16 errCode = 0U;     // 0 = ok；错误码表待 A35 确认
};

struct AlarmEventResult          // A35 主动告警事件（0x0103 草案载荷）
{
    quint8 level = 0U;           // 0=Info 1=Warning 2=Error
    quint16 code = 0U;           // 对端错误码
    quint32 boardTimeMs = 0U;    // 对端源时间戳
    QString text;                // UTF-8 文本
};

// ---- 编码（值域校验失败返回空 QByteArray，调用方不得发送）----
QByteArray encodeServoSet(const ServoSetCmd& cmd);
QByteArray encodeServoMid(quint8 id);
QByteArray encodePropellerSet(const PropellerSetCmd& cmd);
QByteArray encodePropellerStop(quint8 id);
// estop：单帧携带 servoCount 路舵机零值 + thrusterCount 路推进器零值
QByteArray encodeEstop(int servoCount, int thrusterCount);
// horizontal on 统一基准值：单帧 thrusterCount 路相同基准
QByteArray encodeBaseValue(int thrusterCount, qint16 valuePct);
QByteArray encodeHeartbeat(quint32 clientTimeMs);

// ---- 解码（长度/枚举/值域/NaN 校验失败返回 false，调用方丢弃并告警）----
bool decodeSensorSummary(const QByteArray& payload, SensorSummary& out);
bool decodeAck(const QByteArray& payload, AckResult& out);
bool decodeStateEvent(const QByteArray& payload, quint8& stateMask);
bool decodeAlarmEvent(const QByteArray& payload, AlarmEventResult& out);
bool decodeAngleList(const QByteArray& payload, std::vector<qint16>& out);
bool decodePropellerList(const QByteArray& payload, std::vector<qint16>& out);

} // namespace salacia::wire

// 跨线程排队信号传递所需（TcpClient 工作线程 -> 主线程）
Q_DECLARE_METATYPE(salacia::wire::SensorSummary)
