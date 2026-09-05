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

// ----------------------------------------------------------------------------
// 执行器 ID 拓扑（Windows<->A35 协议规范 ID 的唯一权威）
//
//   舵机  wire 0..9   （UI 友好编号 1..10，标签 舵机1（CH0）..舵机10（CH9））
//   垂直  wire 10..13 （UI 组内编号 1..4，标签 垂直1（CH10）..垂直4（CH13））
//   水平  wire 14..15 （UI 组内编号 1..2，标签 水平1（CH14）..水平2（CH15））
//
// 红线：UI 索引/显示编号/wireId 三分离，禁止用 id-1 之类隐式换算兼任协议 ID，
// 一律经下方助手函数映射；Windows 只把这些 ID 视为 Windows<->A35 协议规范 ID，
// 不得引用或解析 M33 协议。
// ----------------------------------------------------------------------------
constexpr int kServoCount = 10;
constexpr quint16 kServoIdFirst = 0U;
constexpr quint16 kServoIdLast = 9U;

constexpr int kVerticalCount = 4;
constexpr quint16 kVerticalIdFirst = 10U;
constexpr quint16 kVerticalIdLast = 13U;

constexpr int kHorizontalCount = 2;
constexpr quint16 kHorizontalIdFirst = 14U;
constexpr quint16 kHorizontalIdLast = 15U;

constexpr int kThrusterCount = kVerticalCount + kHorizontalCount;

constexpr quint8 kIdBroadcast = 0xFFU; // "全部"选择子（仅 mid/stop 类函数）

constexpr bool isValidServoId(quint8 id)
{
    return (id >= kServoIdFirst) && (id <= kServoIdLast);
}

constexpr bool isValidThrusterId(quint8 id)
{
    return (id >= kVerticalIdFirst) && (id <= kHorizontalIdLast);
}

constexpr bool isVerticalThrusterId(quint8 id)
{
    return (id >= kVerticalIdFirst) && (id <= kVerticalIdLast);
}

constexpr bool isHorizontalThrusterId(quint8 id)
{
    return (id >= kHorizontalIdFirst) && (id <= kHorizontalIdLast);
}

// UI 编号 <-> wireId（入参须在组内编号范围内）
constexpr quint8 servoWireId(int uiNumber)
{
    return static_cast<quint8>(static_cast<int>(kServoIdFirst) + uiNumber - 1);
}

constexpr int servoUiNumber(quint8 wireId)
{
    return static_cast<int>(wireId) - static_cast<int>(kServoIdFirst) + 1;
}

constexpr quint8 verticalWireId(int uiNumber)
{
    return static_cast<quint8>(static_cast<int>(kVerticalIdFirst) + uiNumber - 1);
}

constexpr int verticalUiNumber(quint8 wireId)
{
    return static_cast<int>(wireId) - static_cast<int>(kVerticalIdFirst) + 1;
}

constexpr quint8 horizontalWireId(int uiNumber)
{
    return static_cast<quint8>(static_cast<int>(kHorizontalIdFirst) + uiNumber - 1);
}

constexpr int horizontalUiNumber(quint8 wireId)
{
    return static_cast<int>(wireId) - static_cast<int>(kHorizontalIdFirst) + 1;
}

// 扁平序号 1..6（前 4 垂直、后 2 水平）<-> wireId：Phase 15 分组重构前的桥接
constexpr quint8 thrusterWireIdFromFlat(int flatNumber)
{
    return (flatNumber <= kVerticalCount) ? verticalWireId(flatNumber)
                                          : horizontalWireId(flatNumber - kVerticalCount);
}

constexpr int thrusterFlatFromWireId(quint8 wireId)
{
    return static_cast<int>(wireId) - static_cast<int>(kVerticalIdFirst) + 1;
}

// Function ID（集中在唯一注册表，禁散落；estop 与 stop/emergency 严格分离）
enum class Func : quint16
{
    Ask = 0x0001,            // 在线探测
    Ver = 0x0002,            // 版本查询
    Status = 0x0003,         // 系统状态查询
    Help = 0x0004,           // 支持命令查询
    StopAll = 0x0010,        // 六路推进器置零并进入停止状态（复用原 Stop 的 funcId）
    Emergency = 0x0011,      // 紧急停机：仅六路推进器置零，不上浮、不操作舵机
    Estop = 0x0012,          // 一级紧急停机：执行结果与 StopAll 完全相同，仅优先级/
                             // GUI 告警等级/日志事件类型不同；载荷为空，不携带舵机角度
    MoveAll = 0x0013,        // 解除全局停止锁存（推进器总使能 ON）
    StopVertical = 0x0014,   // 垂直组停止锁存（垂直推进使能 OFF）
    MoveVertical = 0x0015,   // 解除垂直组停止锁存（垂直推进使能 ON）
    StopHorizontal = 0x0016, // 水平组停止锁存（水平推进使能 OFF）
    MoveHorizontal = 0x0017, // 解除水平组停止锁存（水平推进使能 ON）
    SafeOn = 0x0020,         // 安全保护模式开（A35 原子保证姿态稳定先开启）
    SafeOff = 0x0021,        // 安全保护模式关（不自动关闭姿态稳定）
    HorizontalOn = 0x0022,   // 姿态稳定（Horizontal）开：Roll/Pitch 自动调平（不代表
                             // 水平推进器），此后推进器按基准值控制
    HorizontalOff = 0x0023,  // 姿态稳定（Horizontal）关：恢复逐路手动
    VerticalSyncOn = 0x0024,    // 垂直组同步开（布局切单条同步滑条）
    VerticalSyncOff = 0x0025,   // 垂直组同步关（恢复逐路滑条）
    HorizontalSyncOn = 0x0026,  // 水平组同步开
    HorizontalSyncOff = 0x0027, // 水平组同步关
    ServoSet = 0x0030,       // set servo <id> <angle>（逐台独立报文，id 0..9）
    ServoSetAll = 0x0031,    // set servo all <angle>
    ServoMid = 0x0032,       // set servo <id|all> mid（id 0..9 或 kIdBroadcast，归中 90°）
    ServoGet = 0x0033,       // get servo <id>（id 0..9）
    ServoGetAll = 0x0034,    // get servo all
    PropellerSet = 0x0040,   // set propeller <id> <value>（逐台独立报文，id 10..15）
    PropellerSetAll = 0x0041,// set propeller all <value>
    PropellerStop = 0x0042,  // set propeller <id|all> stop（id 10..15 或 kIdBroadcast）
    PropellerGetBase = 0x0043, // get propeller <id> base
    PropellerGetReal = 0x0044,// get propeller <id> real
    BaseValue = 0x0050,      // [弃用] 旧统一基准（6 路同值）：勿再使用，改用 BaseValueVH
    BaseValueVH = 0x0051,    // 姿态稳定统一基准：2×i16（垂直基准、水平基准）
    SensorMpu = 0x0060,      // sensor mpu
    SensorDyp = 0x0061,      // sensor dyp（GUI 文案统一 DYP-RD）
    SensorAll = 0x0062,      // sensor all
    Heartbeat = 0x00F0,      // 心跳（机制待 A35 确认）
    SensorSummary = 0x0100,  // 100Hz 传感器汇总（A35 -> Windows）
    Ack = 0x0101,            // ACK/错误响应（回带 seq + errCode）
    StateEvent = 0x0102,     // [legacy] 模式状态事件（4 位掩码，位义冻结，仅兼容回退）
    AlarmEvent = 0x0103,     // 告警事件
    StateEventV2 = 0x0104,   // 模式状态事件 v2（u8 version + u16 mask，9 位权威状态）
};

// 传感器汇总 validMask 位（不用 0 冒充无效值红线）
constexpr quint8 kValidTempHum = 0x01U;
constexpr quint8 kValidMpu = 0x02U;
constexpr quint8 kValidVoltage = 0x04U;
constexpr quint8 kValidDyp = 0x08U;

// DYP-RD 距离无效哨兵（wire 单位暂定 mm，待 A35 确认）
constexpr float kInvalidDistanceMm = -1.0F;

// [legacy] StateEvent(0x0102) 载荷位（u8 掩码：位=1 表示该模式当前处于激活/开启）
// 位义冻结不改；主链路改用 StateEventV2，本组常量仅作兼容回退
constexpr quint8 kStateSafe = 0x01U;
constexpr quint8 kStateHorizontal = 0x02U;
constexpr quint8 kStateEstop = 0x04U;
constexpr quint8 kStateEmergency = 0x08U;

// StateEventV2(0x0104) 载荷：u8 version(=2) + u16 mask（小端）
// stopped 位=1 表示该组处于停止锁存；UI"使能"开关显示取反值
//（如 globalStopped=0 -> 推进器总使能显示 ON）
constexpr quint8 kStateEventV2Version = 2U;
constexpr quint16 kStateV2Safe = 0x0001U;               // bit0 安全保护模式
constexpr quint16 kStateV2AttitudeStab = 0x0002U;       // bit1 姿态稳定（Horizontal）
constexpr quint16 kStateV2GlobalStopped = 0x0004U;      // bit2 全局停止锁存（总使能 OFF）
constexpr quint16 kStateV2VerticalStopped = 0x0008U;    // bit3 垂直组停止锁存
constexpr quint16 kStateV2HorizontalStopped = 0x0010U;  // bit4 水平组停止锁存
constexpr quint16 kStateV2VerticalSync = 0x0020U;       // bit5 垂直组同步
constexpr quint16 kStateV2HorizontalSync = 0x0040U;     // bit6 水平组同步
constexpr quint16 kStateV2Estop = 0x0080U;              // bit7 一级紧急停机激活
constexpr quint16 kStateV2Emergency = 0x0100U;          // bit8 紧急停机激活
constexpr quint16 kStateV2KnownMask = 0x01FFU;          // 已定义位（未知位整帧拒绝）

// 发送优先级（数值小者优先；紧急通道永不排队等待普通控制排空）
// 插队次序红线：Estop > Emergency > Stop/Move > 普通控制
constexpr int kPriorityEstop = 0;
constexpr int kPriorityEmergency = 1;
constexpr int kPriorityStopMove = 2;
constexpr int kPriorityNormal = 5;

} // namespace salacia::wire
