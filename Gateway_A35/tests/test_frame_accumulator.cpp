// Streaming frame accumulator: half/sticky/byte-by-byte feed, resync on bad
// magic/version/CRC, oversize and overflow handling, truncation waits.
#include "test_support.hpp"

#include "wire/crc16.hpp"
#include "wire/frame_accumulator.hpp"

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

// Build a raw header with injected fields (bypasses encodeFrame validation).
std::vector<std::uint8_t> rawHeader(std::uint16_t funcId, std::uint8_t version,
                                    std::uint16_t len)
{
    using namespace gw::wire;
    std::vector<std::uint8_t> h;
    putU32(h, kMagic);
    putU8(h, version);
    putU16(h, funcId);
    putU16(h, 0x1234U);
    putU8(h, kFlagNeedAck);
    putU16(h, len);
    return h;
}

} // namespace

int main()
{
    using namespace gw::wire;

    // Roundtrip through the accumulator.
    {
        const auto payload = makePayload(32U);
        const auto wire = encodeFrame(0x0030U, 0x00FFU, kFlagNeedAck, payload,
                                      kMaxPayloadDefault);
        FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
        acc.feed(wire.data(), wire.size());
        const NextResult r = acc.next();
        CHECK(r.hasFrame);
        CHECK(r.error == FrameError::None);
        CHECK(r.resyncError == FrameError::None);
        CHECK_EQ(r.frame.funcId, static_cast<std::uint16_t>(0x0030U));
        CHECK_EQ(r.frame.seq, static_cast<std::uint16_t>(0x00FFU));
        CHECK_EQ(r.frame.flags, kFlagNeedAck);
        CHECK(r.frame.payload == payload);
        const NextResult again = acc.next();
        CHECK(!again.hasFrame);
        CHECK(again.error == FrameError::None);
    }

    // Half frames: every possible split point.
    {
        const auto payload = makePayload(20U);
        const auto wire =
                encodeFrame(0x00F0U, 7U, 0U, payload, kMaxPayloadDefault);
        for (std::size_t split = 1; split < wire.size(); ++split) {
            FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
            acc.feed(wire.data(), split);
            CHECK(!acc.next().hasFrame); // first half can never complete
            acc.feed(wire.data() + split, wire.size() - split);
            const NextResult r = acc.next();
            CHECK(r.hasFrame);
            CHECK(r.frame.payload == payload);
        }
    }

    // Sticky frames: two frames in one feed.
    {
        const auto p1 = makePayload(8U);
        const auto p2 = makePayload(12U);
        std::vector<std::uint8_t> stream = encodeFrame(0x0030U, 1U, 0U, p1,
                                                       kMaxPayloadDefault);
        const auto second = encodeFrame(0x0040U, 2U, 0U, p2, kMaxPayloadDefault);
        stream.insert(stream.end(), second.begin(), second.end());
        FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
        acc.feed(stream.data(), stream.size());
        const NextResult r1 = acc.next();
        CHECK(r1.hasFrame);
        CHECK_EQ(r1.frame.seq, static_cast<std::uint16_t>(1U));
        CHECK(r1.frame.payload == p1);
        const NextResult r2 = acc.next();
        CHECK(r2.hasFrame);
        CHECK_EQ(r2.frame.seq, static_cast<std::uint16_t>(2U));
        CHECK(r2.frame.payload == p2);
        CHECK(!acc.next().hasFrame);
    }

    // Byte-by-byte feed.
    {
        const auto payload = makePayload(16U);
        const auto wire =
                encodeFrame(0x0012U, 999U, 0U, payload, kMaxPayloadDefault);
        FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
        bool got = false;
        for (std::size_t i = 0; i < wire.size(); ++i) {
            acc.feed(wire.data() + i, 1U);
            const NextResult r = acc.next();
            if (r.hasFrame) {
                got = true;
                CHECK(r.frame.payload == payload);
                CHECK_EQ(r.frame.seq, static_cast<std::uint16_t>(999U));
            }
        }
        CHECK(got);
    }

    // Bad magic prefix: resync drops garbage bytes and recovers the frame.
    {
        const auto payload = makePayload(10U);
        std::vector<std::uint8_t> stream;
        stream.push_back(0x00);
        stream.push_back(0x11);
        stream.push_back(0x22);
        const auto frame =
                encodeFrame(0x0010U, 5U, 0U, payload, kMaxPayloadDefault);
        stream.insert(stream.end(), frame.begin(), frame.end());
        FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
        acc.feed(stream.data(), stream.size());
        bool sawBadMagic = false;
        bool recovered = false;
        for (;;) {
            const NextResult r = acc.next();
            if (r.resyncError == FrameError::BadMagic) {
                sawBadMagic = true;
            }
            if (r.hasFrame) {
                recovered = true;
                CHECK(r.frame.payload == payload);
            }
            if (!r.hasFrame && r.error == FrameError::None) {
                break;
            }
            if (r.error == FrameError::Oversize || r.error == FrameError::Overflow) {
                break;
            }
        }
        CHECK(sawBadMagic);
        CHECK(recovered);
    }

    // Bad version: header with wrong version resyncs, never yields a frame.
    {
        auto stream = rawHeader(0x0001U, 0x2AU, 0U);
        FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
        acc.feed(stream.data(), stream.size());
        bool sawBadVersion = false;
        for (;;) {
            const NextResult r = acc.next();
            CHECK(!r.hasFrame);
            if (r.resyncError == FrameError::BadVersion) {
                sawBadVersion = true;
            }
            if (r.error == FrameError::None && !r.hasFrame) {
                break;
            }
        }
        CHECK(sawBadVersion);
    }

    // Bad CRC: corrupted trailing CRC resyncs, frame is dropped.
    {
        auto wire = encodeFrame(0x0010U, 3U, 0U, makePayload(12U),
                                kMaxPayloadDefault);
        wire[wire.size() - 1U] = static_cast<std::uint8_t>(wire[wire.size() - 1U]
                                                           ^ 0xFF);
        FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
        acc.feed(wire.data(), wire.size());
        bool sawBadCrc = false;
        for (;;) {
            const NextResult r = acc.next();
            CHECK(!r.hasFrame);
            if (r.resyncError == FrameError::BadCrc) {
                sawBadCrc = true;
            }
            if (r.error == FrameError::None && !r.hasFrame) {
                break;
            }
        }
        CHECK(sawBadCrc);
    }

    // Corrupted frame followed by a good frame in the same stream: the good
    // frame survives resync (corruption confined to the first frame).
    {
        auto bad = encodeFrame(0x0010U, 3U, 0U, makePayload(12U),
                               kMaxPayloadDefault);
        bad[bad.size() - 1U] ^= 0x55;
        const auto goodPayload = makePayload(9U);
        const auto good = encodeFrame(0x0020U, 42U, 0U, goodPayload,
                                      kMaxPayloadDefault);
        std::vector<std::uint8_t> stream = bad;
        stream.insert(stream.end(), good.begin(), good.end());
        FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
        acc.feed(stream.data(), stream.size());
        bool sawBadCrc = false;
        bool recovered = false;
        for (;;) {
            const NextResult r = acc.next();
            if (r.resyncError == FrameError::BadCrc) {
                sawBadCrc = true;
            }
            if (r.hasFrame) {
                recovered = true;
                CHECK_EQ(r.frame.seq, static_cast<std::uint16_t>(42U));
                CHECK(r.frame.payload == goodPayload);
            }
            if (r.error == FrameError::None && !r.hasFrame) {
                break;
            }
        }
        CHECK(sawBadCrc);
        CHECK(recovered);
    }

    // Oversize: len beyond maxPayload clears the buffer and reports fatal.
    {
        FrameAccumulator acc(128, kRecvBufferLimitDefault);
        auto stream = rawHeader(0x0001U, kVersion, 200U);
        const auto payload = makePayload(200U);
        stream.insert(stream.end(), payload.begin(), payload.end());
        const std::uint16_t crc = gw::wire::crc16(
                stream.data(), stream.size()); // valid CRC, oversize length
        putU16(stream, crc);
        acc.feed(stream.data(), stream.size());
        const NextResult r = acc.next();
        CHECK(r.error == FrameError::Oversize);
        CHECK(!r.hasFrame);
        CHECK_EQ(acc.buffered(), static_cast<std::size_t>(0U));
    }

    // Truncated frame: keep bytes, wait for the tail.
    {
        const auto wire = encodeFrame(0x0010U, 8U, 0U, makePayload(40U),
                                      kMaxPayloadDefault);
        FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
        acc.feed(wire.data(), wire.size() - 3U);
        const NextResult r = acc.next();
        CHECK(!r.hasFrame);
        CHECK(r.error == FrameError::None);
        CHECK(acc.buffered() > 0U);
    }

    // Overflow: feed beyond the receive limit clears the buffer and the
    // fatal error is surfaced exactly once, then the accumulator recovers.
    {
        FrameAccumulator acc(kMaxPayloadDefault, 64);
        const auto junk = makePayload(100U);
        acc.feed(junk.data(), junk.size());
        CHECK_EQ(acc.buffered(), static_cast<std::size_t>(0U));
        const NextResult r = acc.next();
        CHECK(r.error == FrameError::Overflow);
        CHECK(!r.hasFrame);
        const NextResult after = acc.next();
        CHECK(after.error == FrameError::None);
        CHECK(!after.hasFrame);
        // Recovery: a fresh valid frame parses after the overflow.
        const auto wire = encodeFrame(0x0001U, 1U, 0U, {}, kMaxPayloadDefault);
        acc.feed(wire.data(), wire.size());
        const NextResult ok = acc.next();
        CHECK(ok.hasFrame);
    }

    // reset() drops everything (used on client disconnect).
    {
        const auto wire = encodeFrame(0x0010U, 8U, 0U, makePayload(40U),
                                      kMaxPayloadDefault);
        FrameAccumulator acc(kMaxPayloadDefault, kRecvBufferLimitDefault);
        acc.feed(wire.data(), wire.size() - 2U);
        CHECK(acc.buffered() > 0U);
        acc.reset();
        CHECK_EQ(acc.buffered(), static_cast<std::size_t>(0U));
        const NextResult r = acc.next();
        CHECK(!r.hasFrame);
        CHECK(r.error == FrameError::None);
    }

    TEST_MAIN_END;
}
