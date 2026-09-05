// VideoFrameHub 单元测试：最新帧发布/快照/双读者共享零拷贝/覆盖丢旧/复位
#include <QtTest/qtest.h>

#include <memory>
#include <vector>

#include "video/VideoFrame.h"
#include "video/VideoFrameHub.h"

using namespace salacia;

namespace {

std::shared_ptr<const VideoFrame> makeFrame(int width, int height, std::uint8_t fill)
{
    auto frame = std::make_shared<VideoFrame>();
    frame->width = width;
    frame->height = height;
    frame->bytesPerPixel = 4;
    frame->data.assign(static_cast<std::size_t>(width) * height * 4, fill);
    return frame;
}

} // namespace

class TestVideoHub : public QObject
{
    Q_OBJECT

private slots:
    void initialEmpty();
    void publishSnapshot();
    void overwriteDropsOld();
    void twoReadersShareSameFrame();
    void resetClears();
    void monotonicIndexAcrossPublishes();
};

void TestVideoHub::initialEmpty()
{
    VideoFrameHub hub;
    quint64 index = 123U;
    QVERIFY(hub.takeSnapshot(index) == nullptr);
    QCOMPARE(index, quint64(0));
    QVERIFY(!hub.newerThan(0));
    QVERIFY(!hub.newerThan(100));
}

void TestVideoHub::publishSnapshot()
{
    VideoFrameHub hub;
    const auto frame = makeFrame(64, 48, 0xAB);
    hub.publish(frame, 1U);

    quint64 index = 0;
    std::shared_ptr<const VideoFrame> snap = hub.takeSnapshot(index);
    QVERIFY(snap != nullptr);
    QCOMPARE(index, quint64(1));
    QCOMPARE(snap->width, 64);
    QCOMPARE(snap->height, 48);
    QCOMPARE(snap->data.size(), static_cast<std::size_t>(64 * 48 * 4));
    QVERIFY(snap->data.front() == 0xAB);
    QVERIFY(hub.newerThan(0));
    QVERIFY(!hub.newerThan(1)); // 自身不算更新
}

void TestVideoHub::overwriteDropsOld()
{
    VideoFrameHub hub;
    hub.publish(makeFrame(32, 32, 0x01), 1U);
    hub.publish(makeFrame(32, 32, 0x02), 2U);
    hub.publish(makeFrame(32, 32, 0x03), 3U); // 覆盖旧帧，禁止积压

    quint64 index = 0;
    std::shared_ptr<const VideoFrame> snap = hub.takeSnapshot(index);
    QVERIFY(snap != nullptr);
    QCOMPARE(index, quint64(3));            // 仅剩最新帧
    QVERIFY(snap->data.front() == 0x03);
    QVERIFY(hub.newerThan(2));
    QVERIFY(!hub.newerThan(3));
}

void TestVideoHub::twoReadersShareSameFrame()
{
    // 两个视图（主页/指令页）各自快照：同一 shared_ptr 实例，零拷贝不竞争
    VideoFrameHub hub;
    const auto frame = makeFrame(16, 16, 0x7E);
    hub.publish(frame, 5U);

    quint64 idxA = 0;
    quint64 idxB = 0;
    std::shared_ptr<const VideoFrame> viewA = hub.takeSnapshot(idxA);
    std::shared_ptr<const VideoFrame> viewB = hub.takeSnapshot(idxB);
    QVERIFY(viewA != nullptr);
    QVERIFY(viewA == viewB);   // 共享同一帧对象
    QVERIFY(viewA == frame);   // 与发布对象同一实例（引用计数共享）
    QCOMPARE(idxA, idxB);
    QCOMPARE(idxA, quint64(5));
}

void TestVideoHub::resetClears()
{
    VideoFrameHub hub;
    hub.publish(makeFrame(8, 8, 0x11), 1U);
    hub.reset();
    quint64 index = 99U;
    QVERIFY(hub.takeSnapshot(index) == nullptr);
    QCOMPARE(index, quint64(0));
    QVERIFY(!hub.newerThan(0));
}

void TestVideoHub::monotonicIndexAcrossPublishes()
{
    VideoFrameHub hub;
    for (int i = 1; i <= 10; ++i) {
        hub.publish(makeFrame(4, 4, static_cast<std::uint8_t>(i)), static_cast<quint64>(i));
        quint64 index = 0;
        hub.takeSnapshot(index);
        QCOMPARE(index, static_cast<quint64>(i));
    }
}

QTEST_APPLESS_MAIN(TestVideoHub)
#include "test_videohub.moc"
