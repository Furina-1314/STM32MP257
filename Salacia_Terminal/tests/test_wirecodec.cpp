// WireCodec 单元测试：分帧（半包/粘包/逐字节/多帧）与坏帧处置
#include <QtTest/qtest.h>  // 窄化包含：避开 QtCore 伞头（qthreadpool->qrunnable 的 const 默认构造与 /permissive- 冲突）

#include "communication/WireCodec.h"
#include "communication/WireConstants.h"

using namespace salacia::wire;

namespace {

QByteArray makePayload(int size)
{
    QByteArray p;
    p.reserve(size);
    for (int i = 0; i < size; ++i) {
        p.append(static_cast<char>((i * 7) & 0xFF));
    }
    return p;
}

// 手工构造原始帧头（绕过 encodeFrame 以便注入非法字段）
QByteArray rawHeader(quint16 funcId, quint8 version, quint16 len)
{
    QByteArray h;
    putU32(h, kMagic);
    h.append(static_cast<char>(version));
    putU16(h, funcId);
    putU16(h, 0x1234U);
    h.append(static_cast<char>(kFlagNeedAck));
    putU16(h, len);
    return h;
}

} // namespace

class TestWireCodec : public QObject
{
    Q_OBJECT

private slots:
    void roundtrip()
    {
        const QByteArray payload = makePayload(32);
        const QByteArray wire = encodeFrame(static_cast<quint16>(Func::ServoSet),
                                            0x00FFU, kFlagNeedAck, payload, 4096);
        QVERIFY(!wire.isEmpty());

        FrameAccumulator acc(4096, 65536);
        acc.feed(wire);
        const auto r = acc.next();
        QVERIFY(r.hasFrame);
        QCOMPARE(r.error, FrameError::None);
        QCOMPARE(r.frame.funcId, static_cast<quint16>(Func::ServoSet));
        QCOMPARE(r.frame.seq, 0x00FFU);
        QCOMPARE(r.frame.flags, kFlagNeedAck);
        QCOMPARE(r.frame.payload, payload);
        // 再取应无帧
        const auto r2 = acc.next();
        QVERIFY(!r2.hasFrame);
    }

    void halfSplitEveryByte()
    {
        const QByteArray payload = makePayload(20);
        const QByteArray wire = encodeFrame(static_cast<quint16>(Func::Heartbeat),
                                            7U, 0U, payload, 4096);
        for (int split = 1; split < wire.size(); ++split) {
            FrameAccumulator acc(4096, 65536);
            acc.feed(wire.left(split));
            QVERIFY(!acc.next().hasFrame); // 前半必然不足
            acc.feed(wire.mid(split));
            const auto r = acc.next();
            QVERIFY(r.hasFrame);
            QCOMPARE(r.frame.payload, payload);
        }
    }

    void stickyFrames()
    {
        const QByteArray p1 = makePayload(8);
        const QByteArray p2 = makePayload(12);
        const QByteArray wire = encodeFrame(0x0030U, 1U, 0U, p1, 4096)
                + encodeFrame(0x0040U, 2U, 0U, p2, 4096);
        FrameAccumulator acc(4096, 65536);
        acc.feed(wire);
        const auto r1 = acc.next();
        QVERIFY(r1.hasFrame);
        QCOMPARE(r1.frame.seq, 1U);
        QCOMPARE(r1.frame.payload, p1);
        const auto r2 = acc.next();
        QVERIFY(r2.hasFrame);
        QCOMPARE(r2.frame.seq, 2U);
        QCOMPARE(r2.frame.payload, p2);
        QVERIFY(!acc.next().hasFrame);
    }

    void byteByByte()
    {
        const QByteArray payload = makePayload(16);
        const QByteArray wire = encodeFrame(0x0012U, 999U, 0U, payload, 4096);
        FrameAccumulator acc(4096, 65536);
        bool got = false;
        for (int i = 0; i < wire.size(); ++i) {
            acc.feed(wire.constData() + i, 1);
            const auto r = acc.next();
            if (r.hasFrame) {
                got = true;
                QCOMPARE(r.frame.payload, payload);
            }
        }
        QVERIFY(got);
    }

    void badMagicResync()
    {
        const QByteArray payload = makePayload(10);
        QByteArray garbage;
        garbage.append(static_cast<char>(0x00));
        garbage.append(static_cast<char>(0x11));
        garbage.append(static_cast<char>(0x22));
        const QByteArray wire = garbage
                + encodeFrame(0x0010U, 5U, 0U, payload, 4096);
        FrameAccumulator acc(4096, 65536);
        acc.feed(wire);
        bool sawResync = false;
        FrameAccumulator::NextResult r;
        for (;;) {
            r = acc.next();
            if (r.resyncError == FrameError::BadMagic) {
                sawResync = true;
            }
            if (r.hasFrame || (!r.hasFrame && r.error == FrameError::None)) {
                break;
            }
        }
        QVERIFY(sawResync);
        QVERIFY(r.hasFrame);
        QCOMPARE(r.frame.payload, payload);
    }

    void badVersionRejected()
    {
        QByteArray wire = rawHeader(0x0001U, 0x2AU, 0U); // 错误版本
        FrameAccumulator acc(4096, 65536);
        acc.feed(wire);
        bool sawBadVersion = false;
        for (;;) {
            const auto r = acc.next();
            if (r.hasFrame) {
                QFAIL("bad version must not yield frame");
            }
            if (r.resyncError == FrameError::BadVersion) {
                sawBadVersion = true;
            }
            if (r.error == FrameError::None) {
                break; // 缓冲耗尽
            }
        }
        QVERIFY(sawBadVersion);
    }

    void badCrcRejected()
    {
        QByteArray wire = encodeFrame(0x0010U, 3U, 0U, makePayload(12), 4096);
        wire[wire.size() - 1] = static_cast<char>(wire[wire.size() - 1] ^ 0xFF); // 破坏 CRC
        FrameAccumulator acc(4096, 65536);
        acc.feed(wire);
        bool sawBadCrc = false;
        for (;;) {
            const auto r = acc.next();
            if (r.hasFrame) {
                QFAIL("bad crc must not yield frame");
            }
            if (r.resyncError == FrameError::BadCrc) {
                sawBadCrc = true;
            }
            if (r.error == FrameError::None) {
                break;
            }
        }
        QVERIFY(sawBadCrc);
    }

    void oversizeRejected()
    {
        FrameAccumulator acc(128, 65536); // maxPayload=128
        QByteArray wire = rawHeader(0x0001U, kVersion, 200U); // len=200 超限
        wire.append(makePayload(200));
        const quint16 crc = wireCrc16(wire.constData(), static_cast<int>(wire.size()));
        putU16(wire, crc);
        acc.feed(wire);
        const auto r = acc.next();
        QCOMPARE(r.error, FrameError::Oversize);
        QVERIFY(!r.hasFrame);
        QCOMPARE(acc.buffered(), 0); // 缓存被清空（建议断线重连）
    }

    void truncatedWaits()
    {
        const QByteArray payload = makePayload(40);
        const QByteArray wire = encodeFrame(0x0010U, 8U, 0U, payload, 4096);
        FrameAccumulator acc(4096, 65536);
        acc.feed(wire.left(wire.size() - 3)); // 截断尾部
        QVERIFY(!acc.next().hasFrame);
        QVERIFY(acc.buffered() > 0); // 保留等待补齐
    }

    void overflowClears()
    {
        FrameAccumulator acc(4096, 64); // 极小缓存上限
        acc.feed(makePayload(100));
        QCOMPARE(acc.buffered(), 0); // 溢出即清空
        const auto r = acc.next();
        QVERIFY(!r.hasFrame);
    }

    void seqBoundary()
    {
        const QByteArray wire = encodeFrame(0x00F0U, 65535U, 0U, QByteArray(), 4096);
        FrameAccumulator acc(4096, 65536);
        acc.feed(wire);
        const auto r = acc.next();
        QVERIFY(r.hasFrame);
        QCOMPARE(r.frame.seq, 65535U);
    }

    void payloadCapRejects()
    {
        QVERIFY(encodeFrame(0x0010U, 1U, 0U, makePayload(200), 128).isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestWireCodec)
#include "test_wirecodec.moc"
