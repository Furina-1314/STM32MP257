#ifndef GW_WIRE_FRAME_ACCUMULATOR_HPP
#define GW_WIRE_FRAME_ACCUMULATOR_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wire/wire_codec.hpp"

namespace gw::wire {

// Streaming frame decoder for one TCP connection. Semantics mirror the
// Windows terminal FrameAccumulator (protocol section 1):
//   - half frames / sticky frames / byte-by-byte feed all handled;
//   - bad magic / bad version / bad CRC: drop one byte and resync (transient
//     error reported once per next() call for diagnostics);
//   - oversize (len > maxPayload): buffer cleared, fatal error reported;
//     caller should disconnect the client (deframing lost);
//   - receive-buffer overflow: buffer cleared, fatal error reported once.
enum class FrameError
{
    None = 0,
    BadMagic,   // transient: resynced
    BadVersion, // transient: resynced
    BadCrc,     // transient: resynced
    Oversize,   // fatal: buffer cleared
    Overflow,   // fatal: buffer cleared
};

struct NextResult
{
    FrameError error = FrameError::None;        // fatal outcome of this call
    FrameError resyncError = FrameError::None;  // first transient in this call
    bool hasFrame = false;                      // false when error != None
    WireFrame frame;                            // valid when hasFrame
};

class FrameAccumulator
{
public:
    FrameAccumulator(int maxPayload, int recvBufferLimit);

    void feed(const std::uint8_t* data, std::size_t size);

    // Call repeatedly until hasFrame=false && error=None (buffer drained or
    // waiting for more bytes).
    NextResult next();

    void reset(); // drop buffered bytes (after disconnect)

    std::size_t buffered() const { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_;
    int maxPayload_;
    int recvBufferLimit_;
    bool overflowed_ = false;
};

} // namespace gw::wire

#endif // GW_WIRE_FRAME_ACCUMULATOR_HPP
