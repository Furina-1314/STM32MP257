#pragma once

#include <QtGlobal>

// ============================================================================
// Windows<->A35 TCP 线协议常量（候选草案 v1，待 A35 团队确认后对接实机）
//
// 帧格式（小端、逐字段序列化，禁止裸 struct 内存镜像）：
//   偏移  字段     宽度  说明
//   0     magic    u32   0x53414C41
//   4     version  u8    1
//   5     funcId   u16   见 Func 枚举
//   7     seq      u16   Windows 侧生成递增（0-65535 回绕），响应原样回带
//   9     flags    u8    位域见下
//   10    len      u16   payload 字节数（<= max_payload）
//   12    payload  len   逐字段小端序列化
//   12+len crc16    u16   CRC16-CCITT-FALSE，覆盖偏移 0..11+len
// ============================================================================
namespace salacia::wire {

constexpr quint32 kMagic = 0x53414C41U; // "ALAS"（小端读取为 "SALA" 逐字节）
constexpr quint8 kVersion = 1U;
constexpr int kHeaderBytes = 12;
constexpr int kCrcBytes = 2;

// flags 位域
constexpr quint8 kFlagNeedAck = 0x01U; // 请求需要 ACK
constexpr quint8 kFlagEvent = 0x02U;   // 板端主动事件（不要求 seq 匹配）
constexpr quint8 kFlagError = 0x04U;   // 错误响应

// Function ID（集中在唯一注册表，禁散落；estop 与 stop/emergency 严格分离）
enum class Func : quint16
{
    Ask = 0x0001,            // 在线探测
    Ver = 0x0002,            // 版本查询
    Status = 0x0003,         // 系统状态查询
    Help = 0x0004,           // 支持命令查询
    Stop = 0x0010,           // 正常停止（水平归零，垂直维持策略）
    Emergency = 0x0011,      // 紧急上浮（受控脱险）
    Estop = 0x0012,          // 一级紧急停机：单帧携带 10 舵机 + 6 推进器零值
    SafeOn = 0x0020,         // 安全保护模式开
    SafeOff = 0x0021,        // 安全保护模式关
    HorizontalOn = 0x0022,   // 水平/姿态补偿开（此后推进器值为基准转速）
    HorizontalOff = 0x0023,  // 水平/姿态补偿关（恢复逐路手动）
    ServoSet = 0x0030,       // set servo <id> <angle>（逐台独立报文）
    ServoSetAll = 0x0031,    // set servo all <angle>
    ServoMid = 0x0032,       // set servo <id|all> mid（归中 90°）
    ServoGet = 0x0033,       // get servo <id>
    ServoGetAll = 0x0034,    // get servo all
    PropellerSet = 0x0040,   // set propeller <id> <value>（逐台独立报文）
    PropellerSetAll = 0x0041,// set propeller all <value>
    PropellerStop = 0x0042,  // set propeller <id|all> stop（基准归零）
    PropellerGetBase = 0x0043, // get propeller <id> base
    PropellerGetReal = 0x0044,// get propeller <id> real
    BaseValue = 0x0050,      // horizontal on 专用：单帧 6 路相同基准值
    SensorMpu = 0x0060,      // sensor mpu
    SensorDyp = 0x0061,      // sensor dyp（GUI 文案统一 DYP-RD）
    SensorAll = 0x0062,      // sensor all
    Heartbeat = 0x00F0,      // 心跳（机制待 A35 确认）
    SensorSummary = 0x0100,  // 100Hz 传感器汇总（A35 -> Windows）
    Ack = 0x0101,            // ACK/错误响应（回带 seq + errCode）
    StateEvent = 0x0102,     // 模式状态事件（safe/horizontal/estop/emergency 权威状态）
    AlarmEvent = 0x0103,     // 告警事件
};

// 传感器汇总 validMask 位（不用 0 冒充无效值红线）
constexpr quint8 kValidTempHum = 0x01U;
constexpr quint8 kValidMpu = 0x02U;
constexpr quint8 kValidVoltage = 0x04U;
constexpr quint8 kValidDyp = 0x08U;

// DYP-RD 距离无效哨兵（wire 单位暂定 mm，待 A35 确认）
constexpr float kInvalidDistanceMm = -1.0F;

// StateEvent 载荷位（u8 掩码：位=1 表示该模式当前处于激活/开启）
constexpr quint8 kStateSafe = 0x01U;
constexpr quint8 kStateHorizontal = 0x02U;
constexpr quint8 kStateEstop = 0x04U;
constexpr quint8 kStateEmergency = 0x08U;

// 发送优先级（数值小者优先；紧急通道永不排队等待普通控制排空）
constexpr int kPriorityEstop = 0;
constexpr int kPriorityEmergency = 1;
constexpr int kPriorityNormal = 5;

} // namespace salacia::wire
