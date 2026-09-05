#ifndef GW_WIRE_WIRE_CODEC_HPP
#define GW_WIRE_WIRE_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gw::wire {

// ---------------------------------------------------------------------------
// Windows<->A35 TCP frame format (WINDOWS_A35_INTERFACE.md section 1, final):
//   offset  field   width
//   0       magic   u32   0x53414C41
//   4       version u8    1
//   5       funcId  u16
//   7       seq     u16   Windows-generated; echoed verbatim in ACK
//   9       flags   u8    bit0 NeedAck, bit1 Event, bit2 Error
//   10      len     u16   payload bytes (<= maxPayload)
//   12      payload len   little-endian per-field serialization
//   12+len  crc16   u16   CCITT-FALSE over offsets 0..11+len
// All multi-byte fields are little-endian and serialized per field; raw
// struct mirroring is forbidden on either side.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kMagic = 0x53414C41U; // "ALAS" (LE bytes read "SALA")
constexpr std::uint8_t kVersion = 1U;
constexpr int kHeaderBytes = 12;
constexpr int kCrcBytes = 2;
constexpr int kMaxPayloadDefault = 4096;
constexpr int kRecvBufferLimitDefault = 65536;

constexpr std::uint8_t kFlagNeedAck = 0x01U;
constexpr std::uint8_t kFlagEvent = 0x02U;
constexpr std::uint8_t kFlagError = 0x04U;

struct WireFrame
{
    std::uint16_t funcId = 0U;
    std::uint16_t seq = 0U;
    std::uint8_t flags = 0U;
    std::vector<std::uint8_t> payload;
};

// ---- little-endian payload field helpers ---------------------------------

void putU8(std::vector<std::uint8_t>& out, std::uint8_t v);
void putU16(std::vector<std::uint8_t>& out, std::uint16_t v);
void putU32(std::vector<std::uint8_t>& out, std::uint32_t v);
void putI16(std::vector<std::uint8_t>& out, std::int16_t v);
void putF32(std::vector<std::uint8_t>& out, float v); // IEEE754 bit copy

bool getU8(const std::vector<std::uint8_t>& in, std::size_t offset,
           std::uint8_t& out);
bool getU16(const std::vector<std::uint8_t>& in, std::size_t offset,
            std::uint16_t& out);
bool getU32(const std::vector<std::uint8_t>& in, std::size_t offset,
            std::uint32_t& out);
bool getI16(const std::vector<std::uint8_t>& in, std::size_t offset,
            std::int16_t& out);
bool getF32(const std::vector<std::uint8_t>& in, std::size_t offset,
            float& out);

// ---- frame encode ---------------------------------------------------------

// Returns the complete frame (header + payload + trailing LE CRC). Returns an
// empty vector when payload.size() > maxPayload; callers must not send.
std::vector<std::uint8_t> encodeFrame(std::uint16_t funcId, std::uint16_t seq,
                                      std::uint8_t flags,
                                      const std::vector<std::uint8_t>& payload,
                                      int maxPayload);

} // namespace gw::wire

#endif // GW_WIRE_WIRE_CODEC_HPP
