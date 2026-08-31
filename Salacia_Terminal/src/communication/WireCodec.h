#pragma once

#include <QByteArray>

#include <optional>

#include "WireConstants.h"

// ============================================================================
// TCP 帧编解码与流式分帧（半包/粘包/逐字节分片/连续多帧/坏帧重同步）
//
// 线程模型：纯函数/无状态累积器，可在任意线程使用（TcpClient 工作线程）。
// 校验红线：magic/版本/长度上限/CRC 全部通过才产出帧；坏帧重同步（向后
// 逐字节找下一个 magic），超长帧与缓存上限溢出上报调用方决定断线。
// ============================================================================
namespace salacia::wire {

// ---- payload 小端读写辅助（逐字段序列化，禁裸 struct 镜像）----
void putU16(QByteArray& out, quint16 v);
void putU32(QByteArray& out, quint32 v);
void putI16(QByteArray& out, qint16 v);
void putF32(QByteArray& out, float v);
quint16 getU16(const QByteArray& in, int offset, bool& ok);
quint32 getU32(const QByteArray& in, int offset, bool& ok);
qint16 getI16(const QByteArray& in, int offset, bool& ok);
float getF32(const QByteArray& in, int offset, bool& ok);

// CRC16-CCITT-FALSE（与 UDP 遥测 v2 同一算法，复用 telemetryCrc16）
quint16 wireCrc16(const char* data, int size);

// 解析后的完整帧
struct WireFrame
{
    quint16 funcId = 0U;
    quint16 seq = 0U;
    quint8 flags = 0U;
    QByteArray payload;
};

// 帧错误分类（Oversize/Overflow 需调用方断线重连，其余自动重同步）
enum class FrameError
{
    None = 0,
    BadMagic,    // 已自动重同步（丢 1 字节继续）
    BadVersion,  // 已自动重同步
    BadCrc,      // 已自动重同步
    Oversize,    // len 超过 max_payload：缓存已清空，建议断线重连
    Overflow,    // 缓存超过 recv_buffer_limit：缓存已清空，建议断线重连
};

// 编码完整帧（含头与 CRC）；payload 超过 maxPayload 返回空
QByteArray encodeFrame(quint16 funcId, quint16 seq, quint8 flags,
                       const QByteArray& payload, int maxPayload);

// 流式分帧累积器
class FrameAccumulator
{
public:
    FrameAccumulator(int maxPayload, int recvBufferLimit);

    void feed(const char* data, int size);
    void feed(const QByteArray& chunk);

    struct NextResult
    {
        FrameError error = FrameError::None;      // 致命：Oversize/Overflow；None=正常
        FrameError resyncError = FrameError::None; // 诊断：本次调用内发生的重同步
                                                   //（BadMagic/BadVersion/BadCrc，已恢复）
        bool hasFrame = false;                    // error 非 None 时为 false
        WireFrame frame;                          // hasFrame 时有效
    };
    NextResult next(); // 反复调用直至 hasFrame=false 且 error=None（缓冲不足）

    void reset();      // 清空缓存（断线后调用）
    int buffered() const { return static_cast<int>(buffer_.size()); }

private:
    QByteArray buffer_;
    int maxPayload_;
    int recvBufferLimit_;
};

} // namespace salacia::wire
