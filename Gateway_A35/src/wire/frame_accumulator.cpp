#include "wire/frame_accumulator.hpp"

#include "wire/crc16.hpp"

namespace gw::wire {

FrameAccumulator::FrameAccumulator(int maxPayload, int recvBufferLimit)
    : maxPayload_(maxPayload)
    , recvBufferLimit_(recvBufferLimit)
{
}

void FrameAccumulator::feed(const std::uint8_t* data, std::size_t size)
{
    buffer_.insert(buffer_.end(), data, data + size);
    if (static_cast<int>(buffer_.size()) > recvBufferLimit_) {
        buffer_.clear(); // deframing lost; surfaced by next() as Overflow
        overflowed_ = true;
    }
}

NextResult FrameAccumulator::next()
{
    NextResult result;
    if (overflowed_) {
        overflowed_ = false;
        result.error = FrameError::Overflow;
        return result;
    }
    for (;;) {
        const auto noteResync = [&result](FrameError e) {
            if (result.resyncError == FrameError::None) {
                result.resyncError = e;
            }
        };
        if (buffer_.size() < static_cast<std::size_t>(kHeaderBytes)) {
            return result; // waiting for more bytes
        }

        const std::uint32_t magic = static_cast<std::uint32_t>(buffer_[0])
                | (static_cast<std::uint32_t>(buffer_[1]) << 8)
                | (static_cast<std::uint32_t>(buffer_[2]) << 16)
                | (static_cast<std::uint32_t>(buffer_[3]) << 24);
        if (magic != kMagic) {
            buffer_.erase(buffer_.begin());
            noteResync(FrameError::BadMagic);
            continue;
        }

        if (buffer_[4] != kVersion) {
            buffer_.erase(buffer_.begin());
            noteResync(FrameError::BadVersion);
            continue;
        }

        const std::uint16_t len = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(buffer_[10])
                | (static_cast<std::uint16_t>(buffer_[11]) << 8));
        if (static_cast<int>(len) > maxPayload_) {
            buffer_.clear();
            result.error = FrameError::Oversize;
            return result;
        }

        const std::size_t total = static_cast<std::size_t>(kHeaderBytes)
                + static_cast<std::size_t>(len)
                + static_cast<std::size_t>(kCrcBytes);
        if (buffer_.size() < total) {
            return result; // half frame: keep bytes, wait
        }

        const std::uint16_t expected =
                crc16(buffer_.data(), total - static_cast<std::size_t>(kCrcBytes));
        const std::uint16_t actual = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(buffer_[total - 2U])
                | (static_cast<std::uint16_t>(buffer_[total - 1U]) << 8));
        if (actual != expected) {
            buffer_.erase(buffer_.begin());
            noteResync(FrameError::BadCrc);
            continue;
        }

        result.frame.funcId = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(buffer_[5])
                | (static_cast<std::uint16_t>(buffer_[6]) << 8));
        result.frame.seq = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(buffer_[7])
                | (static_cast<std::uint16_t>(buffer_[8]) << 8));
        result.frame.flags = buffer_[9];
        result.frame.payload.assign(buffer_.begin() + kHeaderBytes,
                                    buffer_.begin()
                                            + static_cast<std::ptrdiff_t>(
                                                    kHeaderBytes + len));
        buffer_.erase(buffer_.begin(),
                      buffer_.begin()
                              + static_cast<std::ptrdiff_t>(total));
        result.error = FrameError::None;
        result.hasFrame = true;
        return result;
    }
}

void FrameAccumulator::reset()
{
    buffer_.clear();
    overflowed_ = false;
}

} // namespace gw::wire
