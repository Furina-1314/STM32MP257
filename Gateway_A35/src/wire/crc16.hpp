#ifndef GW_WIRE_CRC16_HPP
#define GW_WIRE_CRC16_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gw::wire {

// CRC16-CCITT-FALSE: init 0xFFFF, polynomial 0x1021, MSB-first, no
// reflection, no final XOR. Matches the Windows terminal implementation
// (telemetryCrc16) and the protocol document section 1.
// Golden vector: crc16("123456789") == 0x29B1; crc16(empty) == 0xFFFF.
std::uint16_t crc16(const std::uint8_t* data, std::size_t size) noexcept;

inline std::uint16_t crc16(const std::vector<std::uint8_t>& bytes) // helper overload
{
    return crc16(bytes.data(), bytes.size());
}

} // namespace gw::wire

#endif // GW_WIRE_CRC16_HPP
