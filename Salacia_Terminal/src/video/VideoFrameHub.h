#pragma once

#include <QMutex>

#include <memory>
#include <utility>

#include "VideoFrame.h"

namespace salacia {

// "最新视频帧"发布层（LatestFrameStore）：单一显示帧源的共享快照
//
// 红线（二轮提示词 §十）：
//  - 只允许一个 GStreamerPipeline、一个 UDP 视频接收源、一套解码流程；
//  - 主页与指令页两个 VideoGLWidget 不得竞争消费同一 RingBuffer；
//  - 共享层只保存最新帧，允许丢弃旧帧、禁止积压，GUI 线程不得阻塞。
//
// 实现：互斥锁 + 单帧槽；帧数据经 shared_ptr<const VideoFrame> 共享，
// 多视图快照零拷贝（引用计数原子递增）。生产者=GStreamer 显示 appsink
// 回调（流线程），消费者=GUI 线程 33ms 节拍快照。
class VideoFrameHub
{
public:
    // 发布最新帧（任意线程；覆盖旧帧；frameIndex 单调递增且 > 0）
    void publish(std::shared_ptr<const VideoFrame> frame, quint64 frameIndex)
    {
        const QMutexLocker lock(&mutex_);
        latest_ = std::move(frame);
        frameIndex_ = frameIndex;
    }

    // 快照（GUI 线程；无帧时返回空指针，frameIndex 置 0）
    std::shared_ptr<const VideoFrame> takeSnapshot(quint64& frameIndex) const
    {
        const QMutexLocker lock(&mutex_);
        frameIndex = frameIndex_;
        return latest_;
    }

    // 是否存在比 frameIndex 更新的帧（重绘节拍判断，免拷贝）
    bool newerThan(quint64 frameIndex) const
    {
        const QMutexLocker lock(&mutex_);
        return frameIndex_ > frameIndex;
    }

    // 清空（保留给断流重建语义；正常运行不调用）
    void reset()
    {
        const QMutexLocker lock(&mutex_);
        latest_.reset();
        frameIndex_ = 0;
    }

private:
    mutable QMutex mutex_;
    std::shared_ptr<const VideoFrame> latest_;
    quint64 frameIndex_ = 0;
};

} // namespace salacia
