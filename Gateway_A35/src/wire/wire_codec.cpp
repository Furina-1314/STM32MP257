#include "wire/wire_codec.hpp"

#include <cstring>

#include "wire/crc16.hpp"

namespace gw::wire {

void putU8(std::vector<std::uint8_t>& out, std::uint8_t v)
{
    out.push_back(v);
}

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int b = 0; b < 4; ++b) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * b)) & 0xFFU));
    }
}

void putI16(std::vector<std::uint8_t>& out, std::int16_t v)
{
    putU16(out, static_cast<std::uint16_t>(v));
}

void putF32(std::vector<std::uint8_t>& out, float v)
{
    static_assert(sizeof(float) == 4, "IEEE754 32-bit float required");
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32(out, bits);
}

bool getU8(const std::vector<std::uint8_t>& in, std::size_t offset,
           std::uint8_t& out)
{
    if (offset >= in.size()) {
        return false;
    }
    out = in[offset];
    return true;
}

bool getU16(const std::vector<std::uint8_t>& in, std::size_t offset,
            std::uint16_t& out)
{
    if ((offset + 2U) > in.size()) {
        return false;
    }
    out = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(in[offset])
            | static_cast<std::uint16_t>(
                  static_cast<std::uint16_t>(in[offset + 1U]) << 8));
    return true;
}

bool getU32(const std::vector<std::uint8_t>& in, std::size_t offset,
            std::uint32_t& out)
{
    if ((offset + 4U) > in.size()) {
        return false;
    }
    out = 0U;
    for (int b = 3; b >= 0; --b) {
        out = (out << 8) | in[offset + static_cast<std::size_t>(b)];
    }
    return true;
}

bool getI16(const std::vector<std::uint8_t>& in, std::size_t offset,
            std::int16_t& out)
{
    std::uint16_t u = 0U;
    if (!getU16(in, offset, u)) {
        return false;
    }
    out = static_cast<std::int16_t>(u);
    return true;
}

bool getF32(const std::vector<std::uint8_t>& in, std::size_t offset, float& out)
{
    std::uint32_t bits = 0U;
    if (!getU32(in, offset, bits)) {
        return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

std::vector<std::uint8_t> encodeFrame(std::uint16_t funcId, std::uint16_t seq,
                                      std::uint8_t flags,
                                      const std::vector<std::uint8_t>& payload,
                                      int maxPayload)
{
    if (static_cast<int>(payload.size()) > maxPayload) {
        return {};
    }
    std::vector<std::uint8_t> frame;
    frame.reserve(static_cast<std::size_t>(kHeaderBytes) + payload.size()
                  + static_cast<std::size_t>(kCrcBytes));
    putU32(frame, kMagic);
    putU8(frame, kVersion);
    putU16(frame, funcId);
    putU16(frame, seq);
    putU8(frame, flags);
    putU16(frame, static_cast<std::uint16_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    putU16(frame, crc16(frame.data(), frame.size()));
    return frame;
}

} // namespace gw::wire
