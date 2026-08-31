#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "VideoFrame.h"
#include "utils/RingBuffer.h"

namespace salacia {

// GStreamer RTP/H264 低延迟接收管线（Worker-Object 形态的控制面）
//
// 线程模型（多线程规范红线）：
//  - 本对象驻留主线程，仅承载控制（start/stop/restart）与看门狗；
//    数据面全部在 GStreamer 自有线程池内完成；
//  - appsink new-sample 回调（GStreamer 流线程）仅做：拷贝帧 ->
//    无锁 RingBuffer 入队 -> 原子计数，绝不触碰任何 UI；
//  - 总线消息经 gst_message_ref + invokeMethod(Qt::QueuedConnection)
//    投递至主线程处理（errorOccurred/pipelineStateChanged 同理）。
//
// 管线（延迟红线）：
//  udpsrc address=<cfg 可选> buffer-size=2MB caps=application/x-rtp(H264/90000)
//   ! rtpjitterbuffer latency=<cfg> drop-on-latency=true   // 防抖动丢帧
//   ! rtph264depay ! h264parse
//   ! <d3d11h264dec ! d3d11download | avdec_h264>          // 硬解优先
//   ! tee
//   ├─ queue leaky=downstream max-size-buffers=2 ! videoconvert
//   │  ! video/x-raw,format=BGRA ! appsink(显示, sync=false, drop)
//   └─ queue leaky=downstream max-size-buffers=2 ! videoscale ! videoconvert
//      ! video/x-raw,format=RGB,width=<W>,height=<H> ! appsink(AI, sync=false)
//  接收侧低延迟等价于编码端 tune=zerolatency：jitterbuffer 丢过期帧 +
//  小容量漏桶队列 + appsink drop=true + sync=false。
//
// 自愈：总线 ERROR/EOS 或 2 秒无帧 -> 销毁并重建管线（scheduleRestart
// 去重防风暴）；重建期间 videoActive=false（经 DataManager 原子发布）。
class GStreamerPipeline : public QObject
{
    Q_OBJECT

public:
    explicit GStreamerPipeline(QObject* parent = nullptr);
    ~GStreamerPipeline() override;
    Q_DISABLE_COPY(GStreamerPipeline)

    // 构建管线并置 PLAYING（参数快照取自 AppConfig；幂等：已运行先停止）
    bool start();
    // 停止并销毁管线（幂等；回调与总线处理器随之注销）
    void stop();
    // 进程退出专用：停数据流但【不销毁】管线（跳过 Intel 驱动 dispose
    // 路径的退出崩溃，TD-8）；管线对象故意泄漏，内核统一回收
    void stopForExit();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // 显示帧通道：生产者=GStreamer 流线程，消费者=Qt Quick 渲染线程
    RingBuffer<VideoFrame, 4>& displayFrames() { return displayFrames_; }
    // AI 帧通道：生产者=GStreamer 流线程，消费者=推理线程（Phase 4）
    RingBuffer<VideoFrame, 4>& aiFrames() { return aiFrames_; }

signals:
    // 均自主线程排队发出，UI 连接时仍须显式 Qt::QueuedConnection
    void pipelineStateChanged(bool running);
    void errorOccurred(const QString& message);

private:
    bool buildAndPlay();
    void scheduleRestart(int delayMs);
    void handleBusMessage(GstMessage* msg); // 主线程执行
    void onWatchdog();                       // 主线程 500ms
    QString resolveDecoderChain();           // 首选硬解 -> 软解回退（记录是否 D3D11 路径）

    static GstFlowReturn onDisplaySample(GstAppSink* sink, gpointer self);
    static GstFlowReturn onAiSample(GstAppSink* sink, gpointer self);
    static void pullFrameToRing(GstAppSink* sink,
                                RingBuffer<VideoFrame, 4>& ring,
                                std::atomic<quint64>& dropped);
    static GstBusSyncReply busSyncCallback(GstBus* bus, GstMessage* msg,
                                           gpointer self);

    // 配置快照（start() 时自 AppConfig 读取，重建期间复用）
    quint16 port_ = 5000;
    QString bindAddress_;           // [network] host_ip；空 = 0.0.0.0 全接口
    int jitterLatencyMs_ = 40;
    QString preferredDecoder_;
    int aiWidth_ = 640;
    int aiHeight_ = 640;
    bool aiEnabled_ = false;
    bool usingD3d11_ = false; // 解码链是否走 D3D11 显存路径

    // 第三方 C 库边界（GStreamer 引用计数对象，stop() 统一释放）
    GstElement* pipeline_ = nullptr;
    GstAppSink* displaySink_ = nullptr; // 归 pipeline_ 所有
    GstAppSink* aiSink_ = nullptr;      // 归 pipeline_ 所有

    std::atomic<bool> running_{false};
    std::atomic<qint64> lastFrameMs_{0};
    std::atomic<quint64> totalFrames_{0};
    std::atomic<quint64> displayDropped_{0}; // 显示帧环溢出（状态栏"丢帧"数据源）
    std::atomic<quint64> aiDropped_{0};      // AI 帧环溢出（仅日志观察，不进状态栏）

    QTimer watchdog_; // 主线程：2s 无帧自愈 + 1Hz 统计上报
    bool restartScheduled_ = false;
    quint64 statLastTotal_ = 0;
    qint64 statLastMs_ = 0;
    quint64 statLastAiDropped_ = 0;
    qint64 startedAtMs_ = 0; // 本轮管线启动时刻（无包诊断用）
    bool noPacketHintLogged_ = false;

    RingBuffer<VideoFrame, 4> displayFrames_;
    RingBuffer<VideoFrame, 4> aiFrames_;
};

} // namespace salacia
