#include "WireCodec.h"

#include <cstring>

#include "TelemetryPacket.h"

namespace salacia::wire {

namespace {
// 小端字节序辅助（x86/x64 主机即小端；逐字节显式转换保证可移植语义）
std::uint8_t le1(quint16 v, int byte) { return static_cast<std::uint8_t>((v >> (8 * byte)) & 0xFFU); }
std::uint8_t le1(quint32 v, int byte) { return static_cast<std::uint8_t>((v >> (8 * byte)) & 0xFFU); }
} // namespace

void putU16(QByteArray& out, quint16 v)
{
    out.append(static_cast<char>(le1(v, 0)));
    out.append(static_cast<char>(le1(v, 1)));
}

void putU32(QByteArray& out, quint32 v)
{
    for (int b = 0; b < 4; ++b) {
        out.append(static_cast<char>(le1(v, b)));
    }
}

void putI16(QByteArray& out, qint16 v)
{
    putU16(out, static_cast<quint16>(v));
}

void putF32(QByteArray& out, float v)
{
    static_assert(sizeof(float) == 4, "IEEE754 32-bit float required");
    quint32 bits = 0U;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32(out, bits);
}

quint16 getU16(const QByteArray& in, int offset, bool& ok)
{
    if ((offset < 0) || ((offset + 2) > in.size())) {
        ok = false;
        return 0U;
    }
    const auto b0 = static_cast<std::uint8_t>(in.at(offset));
    const auto b1 = static_cast<std::uint8_t>(in.at(offset + 1));
    ok = true;
    return static_cast<quint16>(b0 | (b1 << 8));
}

quint32 getU32(const QByteArray& in, int offset, bool& ok)
{
    if ((offset < 0) || ((offset + 4) > in.size())) {
        ok = false;
        return 0U;
    }
    quint32 v = 0U;
    for (int b = 3; b >= 0; --b) {
        v = (v << 8) | static_cast<std::uint8_t>(in.at(offset + b));
    }
    ok = true;
    return v;
}

qint16 getI16(const QByteArray& in, int offset, bool& ok)
{
    const quint16 u = getU16(in, offset, ok);
    return ok ? static_cast<qint16>(u) : 0;
}

float getF32(const QByteArray& in, int offset, bool& ok)
{
    const quint32 bits = getU32(in, offset, ok);
    if (!ok) {
        return 0.0F;
    }
    float v = 0.0F;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

quint16 wireCrc16(const char* data, int size)
{
    // 复用 UDP 遥测 v2 的同一实现（CCITT-FALSE，初值 0xFFFF，多项式 0x1021）
    return telemetryCrc16(reinterpret_cast<const quint8*>(data),
                          static_cast<std::size_t>(size));
}

QByteArray encodeFrame(quint16 funcId, quint16 seq, quint8 flags,
                       const QByteArray& payload, int maxPayload)
{
    if (payload.size() > maxPayload) {
        return QByteArray();
    }
    QByteArray frame;
    frame.reserve(kHeaderBytes + payload.size() + kCrcBytes);
    putU32(frame, kMagic);
    frame.append(static_cast<char>(kVersion));
    putU16(frame, funcId);
    putU16(frame, seq);
    frame.append(static_cast<char>(flags));
    putU16(frame, static_cast<quint16>(payload.size()));
    frame.append(payload);

    const quint16 crc = wireCrc16(frame.constData(), static_cast<int>(frame.size()));
    putU16(frame, crc);
    return frame;
}

FrameAccumulator::FrameAccumulator(int maxPayload, int recvBufferLimit)
    : maxPayload_(maxPayload)
    , recvBufferLimit_(recvBufferLimit)
{
}

void FrameAccumulator::feed(const char* data, int size)
{
    buffer_.append(data, size);
    if (buffer_.size() > recvBufferLimit_) {
        buffer_.clear(); // 溢出即失步：清空并由调用方断线重连
    }
}

void FrameAccumulator::feed(const QByteArray& chunk)
{
    feed(chunk.constData(), static_cast<int>(chunk.size()));
}

FrameAccumulator::NextResult FrameAccumulator::next()
{
    NextResult result;
    for (;;) {
        const auto noteResync = [&result](FrameError e) {
            if (result.resyncError == FrameError::None) {
                result.resyncError = e; // 保留首个瞬态错误供诊断
            }
        };
        if (buffer_.size() < kHeaderBytes) {
            return result; // 缓冲不足，等后续数据
        }

        // magic 校验：失步则向后逐字节重同步
        bool ok = false;
        const quint32 magic = getU32(buffer_, 0, ok);
        if (!ok || (magic != kMagic)) {
            buffer_.remove(0, 1);
            noteResync(FrameError::BadMagic);
            continue;
        }

        const auto version = static_cast<quint8>(buffer_.at(4));
        if (version != kVersion) {
            buffer_.remove(0, 1);
            noteResync(FrameError::BadVersion);
            continue;
        }

        const quint16 len = getU16(buffer_, 10, ok);
        if (!ok) {
            return result;
        }
        if (len > static_cast<quint16>(maxPayload_)) {
            buffer_.clear(); // 超长帧：拒绝并失步，调用方决定断线
            result.error = FrameError::Oversize;
            return result;
        }

        const int total = kHeaderBytes + static_cast<int>(len) + kCrcBytes;
        if (buffer_.size() < total) {
            result.error = FrameError::None; // 半包：等待（BadMagic 等已被消费）
            return result;
        }

        const quint16 expected = wireCrc16(buffer_.constData(), total - kCrcBytes);
        const quint16 actual = getU16(buffer_, total - kCrcBytes, ok);
        if (!ok || (actual != expected)) {
            buffer_.remove(0, 1);
            noteResync(FrameError::BadCrc);
            continue;
        }

        result.frame.funcId = getU16(buffer_, 5, ok);
        result.frame.seq = getU16(buffer_, 7, ok);
        result.frame.flags = static_cast<quint8>(buffer_.at(9));
        result.frame.payload = QByteArray(buffer_.constData() + kHeaderBytes,
                                          static_cast<int>(len));
        buffer_.remove(0, total);
        result.error = FrameError::None;
        result.hasFrame = true;
        return result;
    }
}

void FrameAccumulator::reset()
{
    buffer_.clear();
}

} // namespace salacia::wire
