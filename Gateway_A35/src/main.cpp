// rov_gateway - phase 1 skeleton (wire layer only).
//
// This build contains the Windows<->A35 protocol layer (frame codec, CRC16,
// streaming accumulator, 42-entry function registry, typed payload codecs).
// It intentionally has no TCP server, no RovControl instance and no sensor
// readers yet; those arrive in phases 2-4. "--check" runs a wire-layer
// self-check (registry completeness + CRC golden vector) and exits nonzero
// on failure so startup scripts can gate on it.

#include <cstdio>
#include <cstring>

#include "wire/crc16.hpp"
#include "wire/function_registry.hpp"

namespace {

int runSelfCheck()
{
    bool ok = true;

    const auto& table = gw::wire::registry();
    // 41 = the Windows terminal's actual registry size (doc's "42" is a
    // documentation off-by-one; see DECISIONS.md D-15).
    if (table.size() != 41U) {
        std::printf("registry size %zu != 41\n", table.size());
        ok = false;
    }
    for (const gw::wire::FunctionEntry& entry : table) {
        if (gw::wire::findFunc(static_cast<std::uint16_t>(entry.funcId)) == nullptr) {
            std::printf("registry lookup failed for %s\n", entry.name);
            ok = false;
        }
    }

    const char kGolden[] = "123456789"; // CRC-16/CCITT-FALSE check value
    const std::uint16_t crc =
            gw::wire::crc16(reinterpret_cast<const std::uint8_t*>(kGolden),
                            std::strlen(kGolden));
    if (crc != 0x29B1U) {
        std::printf("crc16 golden vector failed: 0x%04X != 0x29B1\n", crc);
        ok = false;
    }

    std::printf("self-check: %s (%zu functions registered)\n",
                ok ? "PASS" : "FAIL", table.size());
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--check") == 0) {
            return runSelfCheck();
        }
    }

    std::printf("rov_gateway (phase 1 skeleton): wire layer only, no transport\n");
    std::printf("run with --check for the protocol self-check\n");
    return 0;
}
