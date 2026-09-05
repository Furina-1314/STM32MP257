// CRC16-CCITT-FALSE golden vectors and cross-implementation checks.
#include "test_support.hpp"

#include "wire/crc16.hpp"

#include <cstdio>
#include <vector>

namespace {

// Independent table-driven CCITT-FALSE implementation used to cross-check
// the shipped bitwise version on pseudo-random data (different code path,
// same parameters: init 0xFFFF, poly 0x1021, MSB-first, no xorout).
std::uint16_t refCrc16(const std::uint8_t* data, std::size_t size)
{
    static std::uint16_t table[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256U; ++i) {
            std::uint32_t crc = i << 8;
            for (int bit = 0; bit < 8; ++bit) {
                const std::uint32_t msb = (crc >> 15) & 1U;
                crc = (crc << 1) ^ ((msb != 0U) ? 0x1021U : 0U);
            }
            table[i] = static_cast<std::uint16_t>(crc & 0xFFFFU);
        }
        built = true;
    }
    std::uint32_t crc = 0xFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc = ((crc << 8) & 0xFFFFU)
                ^ table[((crc >> 8) ^ data[i]) & 0xFFU];
    }
    return static_cast<std::uint16_t>(crc & 0xFFFFU);
}

} // namespace

int main()
{
    using gw::wire::crc16;

    // Golden vectors (published CRC-16/CCITT-FALSE check values).
    {
        const char golden[] = "123456789";
        const std::uint16_t crc = crc16(
                reinterpret_cast<const std::uint8_t*>(golden), 9U);
        CHECK_EQ(crc, static_cast<std::uint16_t>(0x29B1));
    }
    { // empty input: register stays at the initial value
        CHECK_EQ(crc16(nullptr, 0U), static_cast<std::uint16_t>(0xFFFF));
        CHECK_EQ(crc16(reinterpret_cast<const std::uint8_t*>(""), 0U),
                 static_cast<std::uint16_t>(0xFFFF));
    }

    // Cross-check bitwise vs table-driven on pseudo-random buffers of many
    // lengths (covers payload sizes used by the protocol: 0..64, 45, 1024).
    std::uint32_t seed = 0x12345678U;
    const auto nextByte = [&seed]() {
        seed = seed * 1103515245U + 12345U;
        return static_cast<std::uint8_t>((seed >> 16) & 0xFFU);
    };
    for (std::size_t size = 0; size <= 64U; ++size) {
        std::vector<std::uint8_t> buf(size);
        for (std::uint8_t& b : buf) {
            b = nextByte();
        }
        CHECK_EQ(crc16(buf.data(), buf.size()),
                 refCrc16(buf.data(), buf.size()));
    }
    {
        std::vector<std::uint8_t> big(1024U);
        for (std::uint8_t& b : big) {
            b = nextByte();
        }
        CHECK_EQ(crc16(big.data(), big.size()),
                 refCrc16(big.data(), big.size()));
    }

    // Property: appending the CRC big-endian (MSB first) leaves zero residue
    // for this unreflected CRC form. (The wire protocol stores the CRC
    // little-endian and compares it directly, which is unaffected.)
    {
        std::vector<std::uint8_t> buf(37U);
        for (std::uint8_t& b : buf) {
            b = nextByte();
        }
        const std::uint16_t crc = crc16(buf.data(), buf.size());
        buf.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
        CHECK_EQ(crc16(buf.data(), buf.size()), static_cast<std::uint16_t>(0));
    }

    TEST_MAIN_END;
}
