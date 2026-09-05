// Typed payload codecs: request parser value domains, response builder
// layouts (45B SensorSummary offsets, StateEventV2, ACK, AlarmEvent, i16
// lists) and the never-NaN/Inf guarantee.
#include "test_support.hpp"

#include "wire/payload_codec.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

std::vector<std::uint8_t> bytesOf(std::initializer_list<std::uint8_t> list)
{
    return std::vector<std::uint8_t>(list);
}

bool allFinite(const gw::wire::SensorSummaryData& d)
{
    using std::isfinite;
    return isfinite(d.tempC) && isfinite(d.humidPct) && isfinite(d.voltage)
            && isfinite(d.distMm) && isfinite(d.accelMps2[0])
            && isfinite(d.accelMps2[1]) && isfinite(d.accelMps2[2])
            && isfinite(d.gyroRadS[0]) && isfinite(d.gyroRadS[1])
            && isfinite(d.gyroRadS[2]);
}

} // namespace

int main()
{
    using namespace gw::wire;
    using P = std::vector<std::uint8_t>;

    // ---- ACK payload -------------------------------------------------------
    {
        const P ack = buildAck(ErrCode::Safety);
        CHECK_EQ(ack.size(), static_cast<std::size_t>(2U));
        CHECK_EQ(ack[0], static_cast<std::uint8_t>(7U));
        CHECK_EQ(ack[1], static_cast<std::uint8_t>(0U));
        CHECK_EQ(buildAck(ErrCode::Ok).size(), static_cast<std::size_t>(2U));
    }

    // ---- request parsers: accept -------------------------------------------
    {
        ServoSetCmd s;
        CHECK(parseServoSet(bytesOf({0x05, 0xB4, 0x00}), s)); // id 5, angle 180
        CHECK_EQ(s.id, static_cast<std::uint8_t>(5U));
        CHECK_EQ(s.angleDeg, static_cast<std::uint16_t>(180U));
        CHECK(parseServoSet(bytesOf({0x00, 0x00, 0x00}), s)); // id 0, angle 0

        std::uint16_t angle = 0U;
        CHECK(parseServoSetAll(bytesOf({0x5A, 0x00}), angle)); // 90
        CHECK_EQ(angle, static_cast<std::uint16_t>(90U));

        std::uint8_t id = 0U;
        CHECK(parseServoMid(bytesOf({0x03}), id));
        CHECK(parseServoMid(bytesOf({0xFF}), id)); // broadcast
        CHECK(parseServoGet(bytesOf({0x09}), id));

        PropellerSetCmd p;
        CHECK(parsePropellerSet(bytesOf({0x0A, 0x9C, 0xFF}), p)); // id10, -100
        CHECK_EQ(p.id, static_cast<std::uint8_t>(10U));
        CHECK_EQ(p.valuePct, static_cast<std::int16_t>(-100));
        CHECK(parsePropellerSet(bytesOf({0x0F, 0x64, 0x00}), p)); // id15, 100
        CHECK_EQ(p.valuePct, static_cast<std::int16_t>(100));

        std::int16_t pct = 0;
        CHECK(parsePropellerSetAll(bytesOf({0x00, 0x00}), pct)); // 0

        std::uint8_t stopId = 0U;
        CHECK(parsePropellerStop(bytesOf({0x0E}), stopId));
        CHECK(parsePropellerStop(bytesOf({0xFF}), stopId));

        std::uint8_t getId = 0U;
        CHECK(parsePropellerGet(bytesOf({0x0A}), getId));
        CHECK(parsePropellerGet(bytesOf({0x0D}), getId));

        BaseValueVhCmd base;
        CHECK(parseBaseValueVh(bytesOf({0x9C, 0xFF, 0x64, 0x00}), base)); // -100,100
        CHECK_EQ(base.verticalPct, static_cast<std::int16_t>(-100));
        CHECK_EQ(base.horizontalPct, static_cast<std::int16_t>(100));

        std::uint32_t hb = 0U;
        CHECK(parseHeartbeat(bytesOf({0x78, 0x56, 0x34, 0x12}), hb));
        CHECK_EQ(hb, 0x12345678U); // little-endian
    }

    // ---- request parsers: reject (length / id / value domain) --------------
    {
        ServoSetCmd s;
        CHECK(!parseServoSet(bytesOf({0x0A, 0x5A, 0x00}), s)); // id 10 invalid
        CHECK(!parseServoSet(bytesOf({0x05, 0xB5, 0x00}), s)); // angle 181
        CHECK(!parseServoSet(bytesOf({0x05, 0x5A}), s));       // short
        CHECK(!parseServoSet(bytesOf({0x05, 0x5A, 0x00, 0x00}), s)); // long

        std::uint16_t angle = 0U;
        CHECK(!parseServoSetAll(bytesOf({0xFF, 0xFF}), angle)); // 65535
        CHECK(!parseServoSetAll(P(1U, 0U), angle));

        std::uint8_t id = 0U;
        CHECK(!parseServoMid(bytesOf({0x0A}), id)); // 10 invalid for mid
        CHECK(!parseServoGet(bytesOf({0xFF}), id)); // broadcast not for get
        CHECK(!parseServoMid(P(), id));

        PropellerSetCmd p;
        CHECK(!parsePropellerSet(bytesOf({0x09, 0x00, 0x00}), p)); // id 9
        CHECK(!parsePropellerSet(bytesOf({0x10, 0x00, 0x00}), p)); // id 16
        CHECK(!parsePropellerSet(bytesOf({0x0A, 0x65, 0x00}), p)); // 101
        CHECK(!parsePropellerSet(bytesOf({0x0A, 0x9B, 0xFF}), p)); // -101

        std::int16_t pct = 0;
        CHECK(!parsePropellerSetAll(bytesOf({0x65, 0x00}), pct));

        std::uint8_t stopId = 0U;
        CHECK(!parsePropellerStop(bytesOf({0x09}), stopId));
        CHECK(!parsePropellerStop(bytesOf({0x00}), stopId));

        std::uint8_t getId = 0U;
        CHECK(!parsePropellerGet(bytesOf({0x16}), getId)); // 22 invalid

        BaseValueVhCmd base;
        CHECK(!parseBaseValueVh(bytesOf({0x65, 0x00, 0x00, 0x00}), base));
        CHECK(!parseBaseValueVh(bytesOf({0x00, 0x00, 0x65, 0x00}), base));
        CHECK(!parseBaseValueVh(P(3U, 0U), base));

        std::uint32_t hb = 0U;
        CHECK(!parseHeartbeat(P(3U, 0U), hb));
        CHECK(!parseHeartbeat(P(5U, 0U), hb));
    }

    // ---- SensorSummary: exact 45-byte layout per protocol section 4 -------
    {
        SensorSummaryData d;
        d.tempC = 26.5F;
        d.humidPct = 56.7F;
        d.accelMps2[0] = 0.1F;
        d.accelMps2[1] = -0.2F;
        d.accelMps2[2] = 9.8F;
        d.gyroRadS[0] = 0.01F;
        d.gyroRadS[1] = 0.02F;
        d.gyroRadS[2] = -0.03F;
        d.voltage = 15.2F;
        d.distMm = 350.0F;
        d.validMask = static_cast<std::uint8_t>(kValidTempHum | kValidMpu
                                                | kValidVoltage | kValidDyp);
        d.boardTimeMs = 12345U;

        const P payload = buildSensorSummary(d);
        CHECK_EQ(payload.size(), static_cast<std::size_t>(45U));

        float f = 0.0F;
        CHECK(getF32(payload, 0U, f) && f == 26.5F);
        CHECK(getF32(payload, 4U, f) && f == 56.7F);
        CHECK(getF32(payload, 8U, f) && f == 0.1F);
        CHECK(getF32(payload, 12U, f) && f == -0.2F);
        CHECK(getF32(payload, 16U, f) && f == 9.8F);
        CHECK(getF32(payload, 20U, f) && f == 0.01F);
        CHECK(getF32(payload, 24U, f) && f == 0.02F);
        CHECK(getF32(payload, 28U, f) && f == -0.03F);
        CHECK(getF32(payload, 32U, f) && f == 15.2F);
        CHECK(getF32(payload, 36U, f) && f == 350.0F);
        CHECK_EQ(payload[40], static_cast<std::uint8_t>(0x0FU));
        std::uint32_t t = 0U;
        CHECK(getU32(payload, 41U, t) && t == 12345U);

        // Cross-check byte order of one field: tempC bits LE.
        std::uint32_t bits = 0U;
        float value = 26.5F;
        std::memcpy(&bits, &value, 4U);
        CHECK_EQ(payload[0], static_cast<std::uint8_t>(bits & 0xFFU));
        CHECK_EQ(payload[3], static_cast<std::uint8_t>((bits >> 24) & 0xFFU));
    }

    // ---- SensorSummary: sanitization keeps the wire finite ----------------
    {
        const float nan_ = std::numeric_limits<float>::quiet_NaN();
        const float inf_ = std::numeric_limits<float>::infinity();

        SensorSummaryData d;
        d.tempC = nan_;
        d.humidPct = 56.0F;
        d.accelMps2[0] = inf_;
        d.gyroRadS[2] = nan_;
        d.voltage = -inf_;
        d.distMm = nan_;
        d.validMask = static_cast<std::uint8_t>(kValidTempHum | kValidMpu
                                                | kValidVoltage | kValidDyp);
        const P payload = buildSensorSummary(d);
        CHECK_EQ(payload.size(), static_cast<std::size_t>(45U));

        // Only the failing bits were cleared.
        CHECK_EQ(d.validMask, static_cast<std::uint8_t>(0U));
        CHECK(allFinite(d));
        // Every float in the wire payload must be finite.
        for (std::size_t off = 0; off + 4U <= 40U; off += 4U) {
            float v = 0.0F;
            CHECK(getF32(payload, off, v));
            CHECK(std::isfinite(v) != 0);
        }
        // Invalid DYP always carries the -1.0 sentinel.
        float dist = 0.0F;
        CHECK(getF32(payload, 36U, dist));
        CHECK_EQ(dist, kInvalidDistanceMm);

        // Partial failure: one bad field clears only its own bit.
        SensorSummaryData ok = d;
        ok.tempC = 25.0F;
        ok.humidPct = 50.0F;
        ok.accelMps2[0] = 0.0F;
        ok.gyroRadS[2] = 0.0F;
        ok.voltage = 12.0F;
        ok.distMm = 100.0F;
        ok.validMask = static_cast<std::uint8_t>(kValidTempHum | kValidMpu
                                                 | kValidVoltage | kValidDyp);
        buildSensorSummary(ok);
        CHECK_EQ(ok.validMask,
                 static_cast<std::uint8_t>(kValidTempHum | kValidMpu
                                           | kValidVoltage | kValidDyp));

        SensorSummaryData part = ok;
        part.voltage = nan_;
        part.validMask = static_cast<std::uint8_t>(kValidTempHum | kValidMpu
                                                   | kValidVoltage | kValidDyp);
        buildSensorSummary(part);
        CHECK_EQ(part.validMask,
                 static_cast<std::uint8_t>(kValidTempHum | kValidMpu | kValidDyp));
        CHECK_EQ(part.voltage, 0.0F); // invalid voltage is zeroed, bit2 clear

        // Invalid-but-finite DYP (bit already clear) also uses the sentinel.
        SensorSummaryData inv = ok;
        inv.distMm = 123.0F;
        inv.validMask = static_cast<std::uint8_t>(kValidTempHum | kValidMpu);
        buildSensorSummary(inv);
        CHECK_EQ(inv.distMm, kInvalidDistanceMm);
    }

    // ---- StateEventV2: version byte, LE mask, per-bit mapping -------------
    {
        StateV2 none;
        const P empty = buildStateEventV2(none);
        CHECK_EQ(empty.size(), static_cast<std::size_t>(3U));
        CHECK_EQ(empty[0], kStateEventV2Version);
        CHECK_EQ(empty[1], static_cast<std::uint8_t>(0U));
        CHECK_EQ(empty[2], static_cast<std::uint8_t>(0U));

        StateV2 all;
        all.safe = true;
        all.attitudeStab = true;
        all.globalStopped = true;
        all.verticalStopped = true;
        all.horizontalStopped = true;
        all.verticalSync = true;
        all.horizontalSync = true;
        all.estop = true;
        all.emergency = true;
        const P full = buildStateEventV2(all);
        CHECK_EQ(full[0], static_cast<std::uint8_t>(2U));
        CHECK_EQ(full[1], static_cast<std::uint8_t>(0xFFU)); // low byte
        CHECK_EQ(full[2], static_cast<std::uint8_t>(0x01U)); // high byte: bit8
        CHECK_EQ((static_cast<std::uint16_t>(full[1])
                  | (static_cast<std::uint16_t>(full[2]) << 8)),
                 kStateV2KnownMask);

        StateV2 one;
        one.emergency = true;
        const P mask = buildStateEventV2(one);
        CHECK_EQ((static_cast<std::uint16_t>(mask[1])
                  | (static_cast<std::uint16_t>(mask[2]) << 8)),
                 kStateV2Emergency);

        StateV2 bits;
        bits.globalStopped = true;
        bits.horizontalSync = true;
        const P m2 = buildStateEventV2(bits);
        CHECK_EQ((static_cast<std::uint16_t>(m2[1])
                  | (static_cast<std::uint16_t>(m2[2]) << 8)),
                 static_cast<std::uint16_t>(kStateV2GlobalStopped
                                            | kStateV2HorizontalSync));
    }

    // ---- AlarmEvent layout --------------------------------------------------
    {
        const P alarm = buildAlarmEvent(1U, 7U, 0xAABBCCDDU, "thrust fault");
        CHECK_EQ(alarm.size(), static_cast<std::size_t>(7U + 12U));
        CHECK_EQ(alarm[0], static_cast<std::uint8_t>(1U));
        CHECK_EQ(alarm[1], static_cast<std::uint8_t>(7U));
        CHECK_EQ(alarm[2], static_cast<std::uint8_t>(0U));
        std::uint32_t t = 0U;
        CHECK(getU32(alarm, 3U, t) && t == 0xAABBCCDDU);
        CHECK_EQ(std::memcmp(alarm.data() + 7U, "thrust fault", 12U), 0);
        // Level > 2 clamps to 2 (Windows rejects otherwise).
        const P clamped = buildAlarmEvent(9U, 0U, 0U, "");
        CHECK_EQ(clamped[0], static_cast<std::uint8_t>(2U));
    }

    // ---- i16 list data payloads ---------------------------------------------
    {
        const P angles = buildAngleList({90, 0, 180});
        CHECK_EQ(angles.size(), static_cast<std::size_t>(6U));
        std::int16_t v = 0;
        CHECK(getI16(angles, 0U, v) && v == 90);
        CHECK(getI16(angles, 2U, v) && v == 0);
        CHECK(getI16(angles, 4U, v) && v == 180);

        CHECK(buildAngleList({90, 181}).empty());
        CHECK(buildAngleList({-1}).empty());

        const P pcts = buildPropellerList({-100, 0, 100});
        CHECK_EQ(pcts.size(), static_cast<std::size_t>(6U));
        CHECK(getI16(pcts, 0U, v) && v == -100);
        CHECK(getI16(pcts, 4U, v) && v == 100);

        CHECK(buildPropellerList({101}).empty());
        CHECK(buildPropellerList({-101}).empty());
    }

    TEST_MAIN_END;
}
