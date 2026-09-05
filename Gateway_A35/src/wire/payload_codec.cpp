#include "wire/payload_codec.hpp"

#include <cmath>
#include <cstring>

#include "wire/function_registry.hpp"

namespace gw::wire {

namespace {

constexpr std::uint16_t kServoAngleMax = 180U;
constexpr std::int16_t kPropellerPctMin = -100;
constexpr std::int16_t kPropellerPctMax = 100;

bool isFinite(float v)
{
    return std::isfinite(v) != 0;
}

} // namespace

// ---- request payload parsers ----------------------------------------------

bool parseServoSet(const std::vector<std::uint8_t>& p, ServoSetCmd& out)
{
    if (p.size() != 3U) {
        return false;
    }
    ServoSetCmd cmd;
    cmd.id = p[0];
    if (!getU16(p, 1U, cmd.angleDeg)) {
        return false;
    }
    if (!isValidServoId(cmd.id) || (cmd.angleDeg > kServoAngleMax)) {
        return false;
    }
    out = cmd;
    return true;
}

bool parseServoSetAll(const std::vector<std::uint8_t>& p, std::uint16_t& angleDeg)
{
    if (p.size() != 2U) {
        return false;
    }
    std::uint16_t angle = 0U;
    if (!getU16(p, 0U, angle) || (angle > kServoAngleMax)) {
        return false;
    }
    angleDeg = angle;
    return true;
}

bool parseServoMid(const std::vector<std::uint8_t>& p, std::uint8_t& id)
{
    if (p.size() != 1U) {
        return false;
    }
    const std::uint8_t value = p[0];
    if (!isValidServoId(value) && (value != kIdBroadcast)) {
        return false;
    }
    id = value;
    return true;
}

bool parseServoGet(const std::vector<std::uint8_t>& p, std::uint8_t& id)
{
    if (p.size() != 1U) {
        return false;
    }
    if (!isValidServoId(p[0])) {
        return false;
    }
    id = p[0];
    return true;
}

bool parsePropellerSet(const std::vector<std::uint8_t>& p, PropellerSetCmd& out)
{
    if (p.size() != 3U) {
        return false;
    }
    PropellerSetCmd cmd;
    cmd.id = p[0];
    if (!getI16(p, 1U, cmd.valuePct)) {
        return false;
    }
    if (!isValidThrusterId(cmd.id) || (cmd.valuePct < kPropellerPctMin)
        || (cmd.valuePct > kPropellerPctMax)) {
        return false;
    }
    out = cmd;
    return true;
}

bool parsePropellerSetAll(const std::vector<std::uint8_t>& p, std::int16_t& valuePct)
{
    if (p.size() != 2U) {
        return false;
    }
    std::int16_t value = 0;
    if (!getI16(p, 0U, value) || (value < kPropellerPctMin)
        || (value > kPropellerPctMax)) {
        return false;
    }
    valuePct = value;
    return true;
}

bool parsePropellerStop(const std::vector<std::uint8_t>& p, std::uint8_t& id)
{
    if (p.size() != 1U) {
        return false;
    }
    if (!isValidPropellerStopId(p[0])) {
        return false;
    }
    id = p[0];
    return true;
}

bool parsePropellerGet(const std::vector<std::uint8_t>& p, std::uint8_t& id)
{
    if (p.size() != 1U) {
        return false;
    }
    if (!isValidThrusterId(p[0])) {
        return false;
    }
    id = p[0];
    return true;
}

bool parseBaseValueVh(const std::vector<std::uint8_t>& p, BaseValueVhCmd& out)
{
    if (p.size() != 4U) {
        return false;
    }
    BaseValueVhCmd cmd;
    if (!getI16(p, 0U, cmd.verticalPct) || !getI16(p, 2U, cmd.horizontalPct)) {
        return false;
    }
    if ((cmd.verticalPct < kPropellerPctMin) || (cmd.verticalPct > kPropellerPctMax)
        || (cmd.horizontalPct < kPropellerPctMin)
        || (cmd.horizontalPct > kPropellerPctMax)) {
        return false;
    }
    out = cmd;
    return true;
}

bool parseHeartbeat(const std::vector<std::uint8_t>& p, std::uint32_t& clientMs)
{
    if (p.size() != 4U) {
        return false;
    }
    return getU32(p, 0U, clientMs);
}

// ---- response/event payload builders ---------------------------------------

std::vector<std::uint8_t> buildAck(ErrCode code)
{
    std::vector<std::uint8_t> payload;
    putU16(payload, static_cast<std::uint16_t>(code));
    return payload;
}

std::vector<std::uint8_t> buildSensorSummary(SensorSummaryData& data)
{
    // Sanitize: a non-finite reading must downgrade to "invalid" for its own
    // bit only; the wire frame must always stay finite.
    if (!isFinite(data.tempC) || !isFinite(data.humidPct)) {
        data.tempC = 0.0F;
        data.humidPct = 0.0F;
        data.validMask = static_cast<std::uint8_t>(data.validMask & ~kValidTempHum);
    }
    bool mpuFinite = true;
    for (int i = 0; i < 3; ++i) {
        if (!isFinite(data.accelMps2[i]) || !isFinite(data.gyroRadS[i])) {
            mpuFinite = false;
        }
    }
    if (!mpuFinite) {
        for (int i = 0; i < 3; ++i) {
            data.accelMps2[i] = 0.0F;
            data.gyroRadS[i] = 0.0F;
        }
        data.validMask = static_cast<std::uint8_t>(data.validMask & ~kValidMpu);
    }
    if (!isFinite(data.voltage)) {
        data.voltage = 0.0F;
        data.validMask = static_cast<std::uint8_t>(data.validMask & ~kValidVoltage);
    }
    if (!isFinite(data.distMm)) {
        data.distMm = kInvalidDistanceMm;
        data.validMask = static_cast<std::uint8_t>(data.validMask & ~kValidDyp);
    }
    if ((data.validMask & kValidDyp) == 0U) {
        data.distMm = kInvalidDistanceMm; // invalid DYP is always the sentinel
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(45U);
    putF32(payload, data.tempC);
    putF32(payload, data.humidPct);
    for (int i = 0; i < 3; ++i) {
        putF32(payload, data.accelMps2[i]);
    }
    for (int i = 0; i < 3; ++i) {
        putF32(payload, data.gyroRadS[i]);
    }
    putF32(payload, data.voltage);
    putF32(payload, data.distMm);
    putU8(payload, data.validMask);
    putU32(payload, data.boardTimeMs);
    return payload;
}

std::vector<std::uint8_t> buildStateEventV2(const StateV2& state)
{
    std::uint16_t mask = 0U;
    if (state.safe) { mask = static_cast<std::uint16_t>(mask | kStateV2Safe); }
    if (state.attitudeStab) { mask = static_cast<std::uint16_t>(mask | kStateV2AttitudeStab); }
    if (state.globalStopped) { mask = static_cast<std::uint16_t>(mask | kStateV2GlobalStopped); }
    if (state.verticalStopped) { mask = static_cast<std::uint16_t>(mask | kStateV2VerticalStopped); }
    if (state.horizontalStopped) { mask = static_cast<std::uint16_t>(mask | kStateV2HorizontalStopped); }
    if (state.verticalSync) { mask = static_cast<std::uint16_t>(mask | kStateV2VerticalSync); }
    if (state.horizontalSync) { mask = static_cast<std::uint16_t>(mask | kStateV2HorizontalSync); }
    if (state.estop) { mask = static_cast<std::uint16_t>(mask | kStateV2Estop); }
    if (state.emergency) { mask = static_cast<std::uint16_t>(mask | kStateV2Emergency); }

    std::vector<std::uint8_t> payload;
    payload.reserve(3U);
    putU8(payload, kStateEventV2Version);
    putU16(payload, mask);
    return payload;
}

std::vector<std::uint8_t> buildAlarmEvent(std::uint8_t level, std::uint16_t code,
                                          std::uint32_t boardTimeMs,
                                          const std::string& utf8Text)
{
    if (level > 2U) {
        level = 2U; // Windows decodeAlarmEvent rejects level > 2
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(7U + utf8Text.size());
    putU8(payload, level);
    putU16(payload, code);
    putU32(payload, boardTimeMs);
    payload.insert(payload.end(), utf8Text.begin(), utf8Text.end());
    return payload;
}

std::vector<std::uint8_t> buildAngleList(const std::vector<std::int16_t>& angles)
{
    std::vector<std::uint8_t> payload;
    for (const std::int16_t v : angles) {
        if ((v < 0) || (v > kServoAngleMax)) {
            return {}; // out of domain: skip data frame entirely
        }
    }
    payload.reserve(angles.size() * 2U);
    for (const std::int16_t v : angles) {
        putI16(payload, v);
    }
    return payload;
}

std::vector<std::uint8_t> buildPropellerList(const std::vector<std::int16_t>& values)
{
    std::vector<std::uint8_t> payload;
    for (const std::int16_t v : values) {
        if ((v < kPropellerPctMin) || (v > kPropellerPctMax)) {
            return {};
        }
    }
    payload.reserve(values.size() * 2U);
    for (const std::int16_t v : values) {
        putI16(payload, v);
    }
    return payload;
}

} // namespace gw::wire
