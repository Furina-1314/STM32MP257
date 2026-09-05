#include "wire/crc16.hpp"

namespace gw::wire {

std::uint16_t crc16(const std::uint8_t* data, std::size_t size) noexcept
{
    std::uint32_t crc = 0xFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t msb = (crc >> 15) & 1U;
            const std::uint32_t mask = (msb != 0U) ? 0x1021U : 0U;
            crc = (crc << 1) ^ mask;
        }
    }
    return static_cast<std::uint16_t>(crc & 0xFFFFU);
}

} // namespace gw::wire
