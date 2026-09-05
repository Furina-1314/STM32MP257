#ifndef GW_WIRE_PAYLOAD_CODEC_HPP
#define GW_WIRE_PAYLOAD_CODEC_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "wire/wire_codec.hpp"

namespace gw::wire {

// ---------------------------------------------------------------------------
// Error codes carried by ACK (0x0101) payloads. Final set per protocol
// section 8; no other values may be produced.
enum class ErrCode : std::uint16_t
{
    Ok = 0,
    BadCmd = 1,
    BadArg = 2,
    Busy = 3,
    NotReady = 4,
    Timeout = 5,
    Unsupported = 6,
    Safety = 7,
};

// ---- request payload parsers (Windows -> A35) -----------------------------
// Every parser validates exact length, id ranges and value domains; false
// means the gateway must ACK ErrCode::BadArg (or BadCmd for unknown ids that
// are structurally impossible here) and never forward the command.

struct ServoSetCmd
{
    std::uint8_t id = 0U;       // wire 0..9
    std::uint16_t angleDeg = 90U; // 0..180
};

struct PropellerSetCmd
{
    std::uint8_t id = 0U;       // wire 10..15
    std::int16_t valuePct = 0;  // -100..100
};

struct BaseValueVhCmd
{
    std::int16_t verticalPct = 0;   // -100..100
    std::int16_t horizontalPct = 0; // -100..100
};

bool parseServoSet(const std::vector<std::uint8_t>& p, ServoSetCmd& out);
bool parseServoSetAll(const std::vector<std::uint8_t>& p, std::uint16_t& angleDeg);
bool parseServoMid(const std::vector<std::uint8_t>& p, std::uint8_t& id); // 0..9 or 0xFF
bool parseServoGet(const std::vector<std::uint8_t>& p, std::uint8_t& id); // 0..9
bool parsePropellerSet(const std::vector<std::uint8_t>& p, PropellerSetCmd& out);
bool parsePropellerSetAll(const std::vector<std::uint8_t>& p, std::int16_t& valuePct);
bool parsePropellerStop(const std::vector<std::uint8_t>& p, std::uint8_t& id); // 10..15 or 0xFF
bool parsePropellerGet(const std::vector<std::uint8_t>& p, std::uint8_t& id);  // 10..15
bool parseBaseValueVh(const std::vector<std::uint8_t>& p, BaseValueVhCmd& out);
bool parseHeartbeat(const std::vector<std::uint8_t>& p, std::uint32_t& clientMs);

// ---- response/event payload builders (A35 -> Windows) ---------------------
// The A35 side owns data-frame format correctness (user directive U-04):
// builders guarantee exact layout, little-endian fields, value domains, and
// never emit NaN/Inf floats.

std::vector<std::uint8_t> buildAck(ErrCode code); // 2B LE

// validMask bits (SensorSummary offset 40).
constexpr std::uint8_t kValidTempHum = 0x01U;
constexpr std::uint8_t kValidMpu = 0x02U;
constexpr std::uint8_t kValidVoltage = 0x04U;
constexpr std::uint8_t kValidDyp = 0x08U;

constexpr float kInvalidDistanceMm = -1.0F; // DYP invalid sentinel

struct SensorSummaryData
{
    float tempC = 0.0F;
    float humidPct = 0.0F;
    float accelMps2[3] = {0.0F, 0.0F, 0.0F};
    float gyroRadS[3] = {0.0F, 0.0F, 0.0F};
    float voltage = 0.0F;
    float distMm = kInvalidDistanceMm;
    std::uint8_t validMask = 0U;
    std::uint32_t boardTimeMs = 0U;
};

// Builds the fixed 45-byte SensorSummary payload (protocol section 4).
// Non-finite inputs are sanitized on the fly: the affected field is zeroed
// (distance becomes kInvalidDistanceMm) and its valid bit is cleared in
// data.validMask, so the wire output is always finite. The caller logs the
// downgrade.
std::vector<std::uint8_t> buildSensorSummary(SensorSummaryData& data);

// StateEventV2 (0x0104) payload: u8 version=2 + u16 LE mask, 9 defined bits
// only (bit9..15 cannot be produced by this struct by construction).
constexpr std::uint8_t kStateEventV2Version = 2U;
constexpr std::uint16_t kStateV2Safe = 0x0001U;               // bit0
constexpr std::uint16_t kStateV2AttitudeStab = 0x0002U;       // bit1
constexpr std::uint16_t kStateV2GlobalStopped = 0x0004U;      // bit2
constexpr std::uint16_t kStateV2VerticalStopped = 0x0008U;    // bit3
constexpr std::uint16_t kStateV2HorizontalStopped = 0x0010U;  // bit4
constexpr std::uint16_t kStateV2VerticalSync = 0x0020U;       // bit5
constexpr std::uint16_t kStateV2HorizontalSync = 0x0040U;     // bit6
constexpr std::uint16_t kStateV2Estop = 0x0080U;              // bit7
constexpr std::uint16_t kStateV2Emergency = 0x0100U;          // bit8
constexpr std::uint16_t kStateV2KnownMask = 0x01FFU;

struct StateV2
{
    bool safe = false;
    bool attitudeStab = false;
    bool globalStopped = false;
    bool verticalStopped = false;
    bool horizontalStopped = false;
    bool verticalSync = false;
    bool horizontalSync = false;
    bool estop = false;
    bool emergency = false;
};

std::vector<std::uint8_t> buildStateEventV2(const StateV2& state); // 3B

// AlarmEvent (0x0103): u8 level (0..2) + u16 code + u32 boardTimeMs + UTF-8
// text. Level out of range is clamped to 2 (Error).
std::vector<std::uint8_t> buildAlarmEvent(std::uint8_t level, std::uint16_t code,
                                          std::uint32_t boardTimeMs,
                                          const std::string& utf8Text);

// Data-frame payloads for query responses (routed to the Windows command
// page via decodeAngleList/decodePropellerList). Any out-of-domain value
// makes the builder return an empty payload: the caller then skips the data
// frame (logs it) so the terminal does not see a frame it must reject.
std::vector<std::uint8_t> buildAngleList(const std::vector<std::int16_t>& angles);      // 0..180
std::vector<std::uint8_t> buildPropellerList(const std::vector<std::int16_t>& values);  // -100..100

} // namespace gw::wire

#endif // GW_WIRE_PAYLOAD_CODEC_HPP
