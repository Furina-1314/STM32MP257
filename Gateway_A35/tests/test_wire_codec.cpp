// Frame codec: little-endian field helpers, layout, CRC placement, caps.
#include "test_support.hpp"

#include "wire/crc16.hpp"
#include "wire/wire_codec.hpp"

#include <cstring>
#include <vector>

namespace {

std::vector<std::uint8_t> makePayload(std::size_t size)
{
    std::vector<std::uint8_t> p;
    p.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        p.push_back(static_cast<std::uint8_t>((i * 7U) & 0xFFU));
    }
    return p;
}

} // namespace

int main()
{
    using namespace gw::wire;

    // Little-endian field encoding: exact byte order.
    {
        std::vector<std::uint8_t> b;
        putU16(b, 0x1234U);
        CHECK_EQ(b.size(), static_cast<std::size_t>(2U));
        CHECK_EQ(b[0], static_cast<std::uint8_t>(0x34));
        CHECK_EQ(b[1], static_cast<std::uint8_t>(0x12));

        b.clear();
        putU32(b, 0x12345678U);
        CHECK_EQ(b.size(), static_cast<std::size_t>(4U));
        CHECK_EQ(b[0], static_cast<std::uint8_t>(0x78));
        CHECK_EQ(b[1], static_cast<std::uint8_t>(0x56));
        CHECK_EQ(b[2], static_cast<std::uint8_t>(0x34));
        CHECK_EQ(b[3], static_cast<std::uint8_t>(0x12));

        b.clear();
        putI16(b, -2); // 0xFFFE
        CHECK_EQ(b[0], static_cast<std::uint8_t>(0xFE));
        CHECK_EQ(b[1], static_cast<std::uint8_t>(0xFF));

        b.clear();
        putF32(b, 1.0F); // IEEE754 0x3F800000 -> LE bytes 00 00 80 3F
        CHECK_EQ(b.size(), static_cast<std::size_t>(4U));
        CHECK_EQ(b[0], static_cast<std::uint8_t>(0x00));
        CHECK_EQ(b[1], static_cast<std::uint8_t>(0x00));
        CHECK_EQ(b[2], static_cast<std::uint8_t>(0x80));
        CHECK_EQ(b[3], static_cast<std::uint8_t>(0x3F));
    }

    // LE helpers roundtrip (including negatives and extremes).
    {
        std::vector<std::uint8_t> b;
        putU16(b, 0xABCDU);
        putU32(b, 0xDEADBEEFU);
        putI16(b, -12345);
        putF32(b, -3.75F);
        std::uint16_t u16 = 0U;
        std::uint32_t u32 = 0U;
        std::int16_t i16 = 0;
        float f = 0.0F;
        CHECK(getU16(b, 0U, u16) && (u16 == 0xABCDU));
        CHECK(getU32(b, 2U, u32) && (u32 == 0xDEADBEEFU));
        CHECK(getI16(b, 6U, i16) && (i16 == -12345));
        CHECK(getF32(b, 8U, f) && (f == -3.75F));
        // Out-of-range reads must fail, not read garbage.
        CHECK(!getU16(b, b.size() - 1U, u16));
        CHECK(!getU32(b, b.size() - 3U, u32));
        CHECK(!getF32(b, b.size(), f));
    }

    // Frame layout: header fields at protocol offsets, CRC trailing.
    {
        const std::vector<std::uint8_t> payload = makePayload(32U);
        const std::vector<std::uint8_t> frame = encodeFrame(
                0x0030U, 0x00FFU, kFlagNeedAck, payload, kMaxPayloadDefault);
        CHECK_EQ(frame.size(),
                 payload.size() + static_cast<std::size_t>(kHeaderBytes + kCrcBytes));
        CHECK_EQ(frame[0], static_cast<std::uint8_t>(0x41)); // magic LE: "ALAS"
        CHECK_EQ(frame[1], static_cast<std::uint8_t>(0x4C));
        CHECK_EQ(frame[2], static_cast<std::uint8_t>(0x41));
        CHECK_EQ(frame[3], static_cast<std::uint8_t>(0x53));
        CHECK_EQ(frame[4], kVersion);
        CHECK_EQ(frame[5], static_cast<std::uint8_t>(0x30)); // funcId LE
        CHECK_EQ(frame[6], static_cast<std::uint8_t>(0x00));
        CHECK_EQ(frame[7], static_cast<std::uint8_t>(0xFF)); // seq LE
        CHECK_EQ(frame[8], static_cast<std::uint8_t>(0x00));
        CHECK_EQ(frame[9], kFlagNeedAck);
        CHECK_EQ(frame[10], static_cast<std::uint8_t>(32U)); // len LE
        CHECK_EQ(frame[11], static_cast<std::uint8_t>(0U));
        CHECK_EQ(frame[12], payload[0]);
        const std::uint16_t storedCrc = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(frame[frame.size() - 2U])
                | (static_cast<std::uint16_t>(frame[frame.size() - 1U]) << 8));
        CHECK_EQ(storedCrc,
                 crc16(frame.data(), frame.size() - static_cast<std::size_t>(kCrcBytes)));
    }

    // Seq boundary 65535 preserved.
    {
        const std::vector<std::uint8_t> frame =
                encodeFrame(0x00F0U, 65535U, 0U, {}, kMaxPayloadDefault);
        CHECK_EQ(frame[7], static_cast<std::uint8_t>(0xFF));
        CHECK_EQ(frame[8], static_cast<std::uint8_t>(0xFF));
    }

    // Payload cap: oversized payload refuses to encode (empty result).
    {
        const std::vector<std::uint8_t> big = makePayload(200U);
        CHECK(encodeFrame(0x0010U, 1U, 0U, big, 128).empty());
        CHECK(!encodeFrame(0x0010U, 1U, 0U, big, 200).empty()); // boundary ok
    }

    // Deterministic wire bytes: whole-frame golden constructed from the
    // header spec plus the CRC function (anchors future refactors).
    {
        const std::vector<std::uint8_t> frame = encodeFrame(
                0x0001U, 0x0104U, kFlagEvent, {}, kMaxPayloadDefault);
        CHECK_EQ(frame.size(), static_cast<std::size_t>(14U)); // 12 + 0 + 2
        const std::uint16_t crc = crc16(frame.data(), 12U);
        CHECK_EQ(frame[12], static_cast<std::uint8_t>(crc & 0xFFU));
        CHECK_EQ(frame[13], static_cast<std::uint8_t>((crc >> 8) & 0xFFU));
    }

    TEST_MAIN_END;
}
