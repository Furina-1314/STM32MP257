#include "FunctionRegistry.h"

#include <cmath>
#include <cstring>

#include "WireCodec.h"

namespace salacia::wire {

namespace {

// 注册表唯一实例（构建期静态初始化；全表条目禁散落红线）
const std::vector<FunctionEntry>& buildTable()
{
    static const std::vector<FunctionEntry> table = {
        // ---- 系统 ----
        {static_cast<quint16>(Func::Ask), "ask", Direction::Request, Category::System,
         true, kPriorityNormal, "linkOnline", "无法确认 A35 在线：显示离线"},
        {static_cast<quint16>(Func::Ver), "ver", Direction::Request, Category::System,
         true, kPriorityNormal, "version", "显示未知版本"},
        {static_cast<quint16>(Func::Status), "status", Direction::Request, Category::System,
         true, kPriorityNormal, "systemState", "显示状态未知"},
        {static_cast<quint16>(Func::Help), "help", Direction::Request, Category::System,
         true, kPriorityNormal, "helpText", "指令页该条目置灰"},
        // ---- 安全（stop/emergency/estop 三语义严格分离）----
        {static_cast<quint16>(Func::Stop), "stop", Direction::Request, Category::Safety,
         true, kPriorityEmergency, "stopState", "无法停止：显示状态未知"},
        {static_cast<quint16>(Func::Emergency), "emergency", Direction::Request, Category::Safety,
         true, kPriorityEmergency, "emergencyState", "无法上浮：显示无法下发"},
        {static_cast<quint16>(Func::Estop), "estop", Direction::Request, Category::Safety,
         true, kPriorityEstop, "estopState", "无 ACK 时不得声称停机成功"},
        // ---- 模式 ----
        {static_cast<quint16>(Func::SafeOn), "safe on", Direction::Request, Category::Mode,
         true, kPriorityNormal, "safeMode", "模式以 A35 状态为准，显示未知"},
        {static_cast<quint16>(Func::SafeOff), "safe off", Direction::Request, Category::Mode,
         true, kPriorityNormal, "safeMode", "模式以 A35 状态为准，显示未知"},
        {static_cast<quint16>(Func::HorizontalOn), "horizontal on", Direction::Request, Category::Mode,
         true, kPriorityNormal, "horizontalMode", "模式以 A35 状态为准，显示未知"},
        {static_cast<quint16>(Func::HorizontalOff), "horizontal off", Direction::Request, Category::Mode,
         true, kPriorityNormal, "horizontalMode", "模式以 A35 状态为准，显示未知"},
        // ---- 舵机 ----
        {static_cast<quint16>(Func::ServoSet), "set servo", Direction::Request, Category::Servo,
         true, kPriorityNormal, "servoState", "超时显示状态未知并查询当前值"},
        {static_cast<quint16>(Func::ServoSetAll), "set servo all", Direction::Request, Category::Servo,
         true, kPriorityNormal, "servoState", "同上"},
        {static_cast<quint16>(Func::ServoMid), "set servo mid", Direction::Request, Category::Servo,
         true, kPriorityNormal, "servoState", "同上"},
        {static_cast<quint16>(Func::ServoGet), "get servo", Direction::Request, Category::Servo,
         true, kPriorityNormal, "servoConfirmed", "无法读取：保持未知"},
        {static_cast<quint16>(Func::ServoGetAll), "get servo all", Direction::Request, Category::Servo,
         true, kPriorityNormal, "servoConfirmed", "无法读取：保持未知"},
        // ---- 推进器 ----
        {static_cast<quint16>(Func::PropellerSet), "set propeller", Direction::Request, Category::Propeller,
         true, kPriorityNormal, "propellerState", "超时显示状态未知并查询当前值"},
        {static_cast<quint16>(Func::PropellerSetAll), "set propeller all", Direction::Request, Category::Propeller,
         true, kPriorityNormal, "propellerState", "同上"},
        {static_cast<quint16>(Func::PropellerStop), "set propeller stop", Direction::Request, Category::Propeller,
         true, kPriorityNormal, "propellerState", "同上"},
        {static_cast<quint16>(Func::PropellerGetBase), "get propeller base", Direction::Request, Category::Propeller,
         true, kPriorityNormal, "propellerConfirmed", "无法读取：保持未知"},
        {static_cast<quint16>(Func::PropellerGetReal), "get propeller real", Direction::Request, Category::Propeller,
         true, kPriorityNormal, "propellerConfirmed", "无法读取：保持未知"},
        // ---- 统一基准（horizontal on 专用多设备单帧）----
        {static_cast<quint16>(Func::BaseValue), "base value", Direction::Request, Category::Propeller,
         true, kPriorityNormal, "baseValueState", "超时显示状态未知"},
        // ---- 传感器查询 ----
        {static_cast<quint16>(Func::SensorMpu), "sensor mpu", Direction::Request, Category::Sensor,
         true, kPriorityNormal, "mpuSnapshot", "显示未就绪"},
        {static_cast<quint16>(Func::SensorDyp), "sensor dyp", Direction::Request, Category::Sensor,
         true, kPriorityNormal, "dypSnapshot", "显示未就绪（DYP-RD）"},
        {static_cast<quint16>(Func::SensorAll), "sensor all", Direction::Request, Category::Sensor,
         true, kPriorityNormal, "sensorReadyState", "显示状态未知"},
        // ---- 链路 ----
        {static_cast<quint16>(Func::Heartbeat), "heartbeat", Direction::Request, Category::Link,
         false, kPriorityNormal, "linkAlive", "按超时判定离线"},
        // ---- A35 -> Windows ----
        {static_cast<quint16>(Func::SensorSummary), "sensor summary", Direction::Telemetry, Category::Sensor,
         false, kPriorityNormal, "sensorSnapshot", "丢弃坏帧并告警"},
        {static_cast<quint16>(Func::Ack), "ack", Direction::Response, Category::Link,
         false, kPriorityNormal, "requestResult", "超时路径处理"},
        {static_cast<quint16>(Func::StateEvent), "state event", Direction::Event, Category::Mode,
         false, kPriorityNormal, "authorityState", "丢弃坏帧并告警"},
        {static_cast<quint16>(Func::AlarmEvent), "alarm event", Direction::Event, Category::System,
         false, kPriorityNormal, "alarmRaised", "丢弃坏帧并告警"},
    };
    return table;
}

bool isFinite(float v)
{
    return std::isfinite(v) != 0;
}

} // namespace

const std::vector<FunctionEntry>& FunctionRegistry::all()
{
    return buildTable();
}

const FunctionEntry* FunctionRegistry::findByFuncId(quint16 funcId)
{
    for (const FunctionEntry& entry : buildTable()) {
        if (entry.funcId == funcId) {
            return &entry;
        }
    }
    return nullptr;
}

const FunctionEntry* FunctionRegistry::findByName(const QString& name)
{
    for (const FunctionEntry& entry : buildTable()) {
        if (name == QLatin1String(entry.name)) {
            return &entry;
        }
    }
    return nullptr;
}

int FunctionRegistry::count()
{
    return static_cast<int>(buildTable().size());
}

// ---------------------------------------------------------------- 编码

QByteArray encodeServoSet(const ServoSetCmd& cmd)
{
    if (cmd.angleDeg > 180U) {
        return QByteArray(); // 值域红线：0-180°
    }
    QByteArray payload;
    payload.append(static_cast<char>(cmd.id));
    putU16(payload, cmd.angleDeg);
    return payload;
}

QByteArray encodeServoMid(quint8 id)
{
    QByteArray payload;
    payload.append(static_cast<char>(id));
    return payload;
}

QByteArray encodePropellerSet(const PropellerSetCmd& cmd)
{
    if ((cmd.valuePct < -100) || (cmd.valuePct > 100)) {
        return QByteArray(); // 值域红线：-100..+100
    }
    QByteArray payload;
    payload.append(static_cast<char>(cmd.id));
    putI16(payload, cmd.valuePct);
    return payload;
}

QByteArray encodePropellerStop(quint8 id)
{
    QByteArray payload;
    payload.append(static_cast<char>(id));
    return payload;
}

QByteArray encodeEstop(int servoCount, int thrusterCount)
{
    if ((servoCount <= 0) || (servoCount > 16) || (thrusterCount <= 0) || (thrusterCount > 16)) {
        return QByteArray();
    }
    // 单帧携带全部零值：舵机 u16 角度 0 + 推进器 i16 基准 0（多设备单帧仅此与基准值两处）
    QByteArray payload;
    for (int i = 0; i < servoCount; ++i) {
        putU16(payload, 0U);
    }
    for (int i = 0; i < thrusterCount; ++i) {
        putI16(payload, 0);
    }
    return payload;
}

QByteArray encodeBaseValue(int thrusterCount, qint16 valuePct)
{
    if ((thrusterCount <= 0) || (thrusterCount > 16)
        || (valuePct < -100) || (valuePct > 100)) {
        return QByteArray();
    }
    QByteArray payload;
    for (int i = 0; i < thrusterCount; ++i) {
        putI16(payload, valuePct);
    }
    return payload;
}

QByteArray encodeHeartbeat(quint32 clientTimeMs)
{
    QByteArray payload;
    putU32(payload, clientTimeMs);
    return payload;
}

// ---------------------------------------------------------------- 解码

bool decodeSensorSummary(const QByteArray& payload, SensorSummary& out)
{
    // 10×f32（温/湿/加速度3/陀螺3/电压/距离）+ u8 + u32 = 45 字节（草案 §3.2）
    constexpr int kExpected = 10 * 4 + 1 + 4;
    if (payload.size() != kExpected) {
        return false;
    }
    bool ok = true;
    SensorSummary s;
    int off = 0;
    s.tempC = getF32(payload, off, ok); off += 4;
    s.humidPct = getF32(payload, off, ok); off += 4;
    for (int i = 0; i < 3; ++i) { s.accelMps2[i] = getF32(payload, off, ok); off += 4; }
    for (int i = 0; i < 3; ++i) { s.gyroRadS[i] = getF32(payload, off, ok); off += 4; }
    s.voltage = getF32(payload, off, ok); off += 4;
    s.distMm = getF32(payload, off, ok); off += 4;
    s.validMask = static_cast<quint8>(payload.at(off)); off += 1;
    s.boardTimeMs = getU32(payload, off, ok);
    if (!ok) {
        return false;
    }
    // NaN/Inf 红线：任一有效字段非有限值即拒绝整帧
    if (!isFinite(s.tempC) || !isFinite(s.humidPct) || !isFinite(s.voltage)
        || !isFinite(s.distMm)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!isFinite(s.accelMps2[i]) || !isFinite(s.gyroRadS[i])) {
            return false;
        }
    }
    out = s;
    return true;
}

bool decodeAck(const QByteArray& payload, AckResult& out)
{
    if (payload.size() != 2) {
        return false;
    }
    bool ok = false;
    const quint16 err = getU16(payload, 0, ok);
    if (!ok) {
        return false;
    }
    out.errCode = err;
    return true;
}

bool decodeStateEvent(const QByteArray& payload, quint8& stateMask)
{
    if (payload.size() != 1) {
        return false;
    }
    const quint8 mask = static_cast<quint8>(payload.at(0));
    if ((mask & ~(kStateSafe | kStateHorizontal | kStateEstop | kStateEmergency)) != 0U) {
        return false; // 未知位：载荷版本不符，整帧拒绝
    }
    stateMask = mask;
    return true;
}

bool decodeAlarmEvent(const QByteArray& payload, AlarmEventResult& out)
{
    // 草案：u8 level + u16 code + u32 boardTimeMs + UTF-8 text（可变长）
    if (payload.size() < 7) {
        return false;
    }
    bool ok = false;
    AlarmEventResult r;
    r.level = static_cast<quint8>(payload.at(0));
    if (r.level > 2U) {
        return false;
    }
    r.code = getU16(payload, 1, ok);
    if (!ok) {
        return false;
    }
    r.boardTimeMs = getU32(payload, 3, ok);
    if (!ok) {
        return false;
    }
    r.text = QString::fromUtf8(payload.mid(7));
    out = r;
    return true;
}

bool decodeAngleList(const QByteArray& payload, std::vector<qint16>& out)
{
    if ((payload.size() < 2) || ((payload.size() % 2) != 0)) {
        return false;
    }
    std::vector<qint16> list;
    for (int off = 0; off + 2 <= payload.size(); off += 2) {
        bool ok = false;
        const qint16 v = getI16(payload, off, ok);
        if (!ok || (v < 0) || (v > 180)) {
            return false; // 角度值域
        }
        list.push_back(v);
    }
    out = list;
    return true;
}

bool decodePropellerList(const QByteArray& payload, std::vector<qint16>& out)
{
    if ((payload.size() < 2) || ((payload.size() % 2) != 0)) {
        return false;
    }
    std::vector<qint16> list;
    for (int off = 0; off + 2 <= payload.size(); off += 2) {
        bool ok = false;
        const qint16 v = getI16(payload, off, ok);
        if (!ok || (v < -100) || (v > 100)) {
            return false; // 百分比值域
        }
        list.push_back(v);
    }
    out = list;
    return true;
}

} // namespace salacia::wire
