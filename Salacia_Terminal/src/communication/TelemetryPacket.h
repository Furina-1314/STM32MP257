#pragma once

#include <QtGlobal>

#include <cstdint>
#include <cstring>

namespace salacia {

// ============================================================================
// 遥测线协议草案 v1（TD-3，板端待实现——按此格式发送）
//
// 传输：UDP 单播 -> 岸基终端 [rov] telemetry_port（默认 5001），20Hz
// 字节序：小端（x86 主机序直读）；整个报文定长 46 字节，packed
//
//   偏移  字段             类型      说明
//   0     magic            u16       0xA55A
//   2     version          u8        协议版本（当前 1）
//   3     flags            u8        位域：bit0 加速度有效 bit1 陀螺有效
//   4     sequence         u32       序号（回绕）
//   8     boardTimeMs      u32       板端时间戳
//   12    accelMps2[3]     f32 x3    加速度（m/s^2，机体系）
//   24    gyroRadS[3]      f32 x3    角速度（rad/s，机体系）
//   36    depthM           f32       深度（米）
//   40    temperatureC     f32       温度（摄氏度）
//   44    crc16            u16       CRC16-CCITT-FALSE（初值 0xFFFF，
//                                    多项式 0x1021，覆盖偏移 0~43）
// ============================================================================

constexpr quint16 kTelemetryMagic = 0xA55AU;
constexpr quint8 kTelemetryVersion = 1U;

#pragma pack(push, 1)
struct TelemetryWirePacket
{
    quint16 magic;
    quint8 version;
    quint8 flags;
    quint32 sequence;
    quint32 boardTimeMs;
    float accelMps2[3];
    float gyroRadS[3];
    float depthM;
    float temperatureC;
    quint16 crc16;
};
#pragma pack(pop)

static_assert(sizeof(TelemetryWirePacket) == 46, "telemetry wire size must be 46");

// 解析后的有效样本（含岸基接收时刻）
struct RawImuSample
{
    quint64 hostTimeMs = 0;
    quint32 boardTimeMs = 0;
    quint16 sequence = 0;
    float accelMps2[3] = {0.0F, 0.0F, 0.0F};
    float gyroRadS[3] = {0.0F, 0.0F, 0.0F};
    float depthM = 0.0F;
    float temperatureC = 0.0F;
};

// CRC16-CCITT-FALSE（初值 0xFFFF，多项式 0x1021，逐位实现）
inline quint16 telemetryCrc16(const quint8* data, std::size_t length)
{
    quint32 crc = 0xFFFFU;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= static_cast<quint32>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            const quint32 msb = (crc >> 15) & 1U;
            const quint32 mask = (msb != 0U) ? 0x1021U : 0U;
            crc = (crc << 1) ^ mask;
        }
    }
    return static_cast<quint16>(crc & 0xFFFFU);
}

// 报文校验与解析（网络输入边界校验红线）：
// 长度/魔数/版本/CRC 全部通过才产出样本；失败返回 false
inline bool parseTelemetryPacket(const char* data, qint64 size, RawImuSample& out)
{
    if (data == nullptr || size != static_cast<qint64>(sizeof(TelemetryWirePacket))) {
        return false;
    }

    TelemetryWirePacket pkt;
    std::memcpy(&pkt, data, sizeof(pkt)); // 小端主机直读（x86/x64）

    if ((pkt.magic != kTelemetryMagic) || (pkt.version != kTelemetryVersion)) {
        return false;
    }
    const quint16 expected =
            telemetryCrc16(reinterpret_cast<const quint8*>(&pkt), sizeof(pkt) - 2U);
    if (pkt.crc16 != expected) {
        return false;
    }

    out.hostTimeMs = 0U; // 由接收方填充
    out.boardTimeMs = pkt.boardTimeMs;
    out.sequence = static_cast<quint16>(pkt.sequence & 0xFFFFU);
    out.accelMps2[0] = pkt.accelMps2[0];
    out.accelMps2[1] = pkt.accelMps2[1];
    out.accelMps2[2] = pkt.accelMps2[2];
    out.gyroRadS[0] = pkt.gyroRadS[0];
    out.gyroRadS[1] = pkt.gyroRadS[1];
    out.gyroRadS[2] = pkt.gyroRadS[2];
    out.depthM = pkt.depthM;
    out.temperatureC = pkt.temperatureC;
    return true;
}

} // namespace salacia
