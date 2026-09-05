#include "GStreamerPipeline.h"
#include <gst/video/video.h>

#include <QDateTime>
#include <QFile>
#include <QStringList>

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "core/AppConfig.h"
#include "core/DataManager.h"
#include "core/Logger.h"

namespace salacia {

namespace {
// GStreamer 全局初始化仅一次（进程级）
std::once_flag gGstOnce;
} // namespace

GStreamerPipeline::GStreamerPipeline(QObject* parent)
    : QObject(parent)
{
    watchdog_.setInterval(AppConfig::instance().videoWatchdogIntervalMs());
    connect(&watchdog_, &QTimer::timeout, this, &GStreamerPipeline::onWatchdog);
}

GStreamerPipeline::~GStreamerPipeline()
{
    stop(); // 逆序安全退出：先于成员 RingBuffer 析构停止数据面
}

bool GStreamerPipeline::start()
{
    std::call_once(gGstOnce, [] { gst_init(nullptr, nullptr); });

    if (isRunning()) {
        stop();
    }

    // 配置快照（参数解耦红线：全部来自 app_config.ini）
    const AppConfig& cfg = AppConfig::instance();
    port_ = cfg.videoRtpPort();
    bindAddress_ = AppConfig::resolveBindAddress(cfg.hostIp());
    jitterLatencyMs_ = cfg.jitterLatencyMs();
    preferredDecoder_ = cfg.preferredDecoder();
    aiEnabled_ = cfg.aiEnabled();
    if (aiEnabled_ && !QFile::exists(cfg.resolvedModelPath())) {
        // 模型未就位：整条 AI 分支不建（省 GPU/CPU，帧环不存在也就无溢出丢帧）
        Logger::warning(QString::fromLocal8Bit("视频：AI 模型缺失（%1），本轮不构建 AI 分支"
                                               "（模型放置后重启程序生效）")
                            .arg(cfg.modelPath()));
        aiEnabled_ = false;
    }
    aiWidth_ = cfg.inputWidth();
    aiHeight_ = cfg.inputHeight();

    return buildAndPlay();
}

void GStreamerPipeline::stop()
{
    if (pipeline_ != nullptr) {
        // 先摘除总线同步处理器，再改状态：避免拆卸期间流线程回调重入
        GstBus* bus = gst_element_get_bus(pipeline_);
        if (bus != nullptr) {
            gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
            gst_object_unref(bus);
        }
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    displaySink_ = nullptr;
    aiSink_ = nullptr;

    watchdog_.stop();
    if (running_.exchange(false, std::memory_order_acq_rel)) {
        DataManager::instance().setVideoActive(false);
        emit pipelineStateChanged(false);
    }
}

QString GStreamerPipeline::resolveDecoderChain()
{
    // 硬解优先（D3D11 需要 d3d11download 转系统内存），失败回退软解
    if (preferredDecoder_ == QStringLiteral("d3d11h264dec")
        && gst_element_factory_find("d3d11h264dec") != nullptr
        && gst_element_factory_find("d3d11download") != nullptr) {
        usingD3d11_ = true; // tee 前保持 D3D11 显存，缩放/转换尽量在 GPU 完成
        return QStringLiteral("d3d11h264dec ! d3d11upload");
    }
    usingD3d11_ = false;
    if (gst_element_factory_find(preferredDecoder_.toUtf8().constData()) != nullptr) {
        return preferredDecoder_;
    }
    if (gst_element_factory_find("avdec_h264") != nullptr) {
        Logger::warning(QString::fromLocal8Bit("视频：首选解码器 %1 不可用，回退 avdec_h264 软解")
                            .arg(preferredDecoder_));
        return QStringLiteral("avdec_h264");
    }
    return QString();
}

bool GStreamerPipeline::buildAndPlay()
{
    const QString decoder = resolveDecoderChain();
    if (decoder.isEmpty()) {
        Logger::error(QString::fromLocal8Bit("视频：无可用 H264 解码器，管线构建失败"));
        emit errorOccurred(QString::fromLocal8Bit("无可用 H264 解码器"));
        return false;
    }

    // 绑定地址子句：[network] host_ip 非空时绑定指定网卡（板端定向推流），
    // 留空 = 0.0.0.0 全接口；接收套接字缓冲（[video] socket_buffer_bytes，
    // 抗 4Mbps 码流包突发防内核丢包）
    const AppConfig& cfg = AppConfig::instance();
    const QString bindClause = bindAddress_.isEmpty()
            ? QString()
            : QString::fromLatin1("address=%1 ").arg(bindAddress_);
    QStringList parts;
    parts << QString::fromLatin1("udpsrc %1port=%2 buffer-size=%3 "
                                 "caps=application/x-rtp,media=(string)video,"
                                 "clock-rate=(int)90000,encoding-name=(string)H264")
                 .arg(bindClause, QString::number(port_),
                      QString::number(cfg.videoSocketBufferBytes()))
          << QString::fromLatin1("rtpjitterbuffer latency=%1 drop-on-latency=true")
                 .arg(jitterLatencyMs_)
          << QString::fromLatin1("rtph264depay ! h264parse")
          << QString::fromLatin1("%1 ! tee name=t").arg(decoder)
          // 显示分支：BGRA 供 Qt Quick 渲染
          << QString::fromLatin1("t. ! queue leaky=downstream max-size-buffers=2 %1"
                                 "! videoconvert ! video/x-raw,format=BGRA "
                                 "! appsink name=displaysink sync=false max-buffers=1 drop=true")
                 .arg(usingD3d11_ ? QString::fromLatin1("! d3d11download ") : QString());
    if (aiEnabled_) {
        // AI 分支（红线：缩放在 GPU 完成）——D3D11 路径用 d3d11convert 显存内
        // 缩放+转 RGBA 后下载，CPU 仅做 640x6640 小图重排；软解回退用 videoscale
        const QString aiConvert = usingD3d11_
                ? QString::fromLatin1("! d3d11convert "
                                      "! video/x-raw(memory:D3D11Memory),format=RGBA,width=%1,height=%2 "
                                      "! d3d11download ! videoconvert ")
                      .arg(aiWidth_).arg(aiHeight_)
                : QString::fromLatin1("! videoscale ! videoconvert ");
        parts << QString::fromLatin1("t. ! queue leaky=downstream max-size-buffers=2 %1"
                                     "! video/x-raw,format=RGB,width=%2,height=%3 "
                                     "! appsink name=aisink sync=false max-buffers=1 drop=true")
                     .arg(aiConvert)
                     .arg(aiWidth_)
                     .arg(aiHeight_);
    }
    // gst-launch 语法：tee 的分支以 "t." 开头、以空格与前一元件衔接；
    // 写成 "name=t ! t." 会形成自链（not-linked），"appsink ! t." 是反向链
    const QString desc = parts.join(QString::fromLatin1(" ! "))
            .replace(QString::fromLatin1("name=t ! t."), QString::fromLatin1("name=t t."))
            .replace(QString::fromLatin1("drop=true ! t."), QString::fromLatin1("drop=true t."));

    GError* parseError = nullptr;
    GstElement* pipeline = gst_parse_launch(desc.toUtf8().constData(), &parseError);
    if (pipeline == nullptr) {
        const QString text = QString::fromLocal8Bit("视频：管线解析失败：%1")
                                 .arg(QString::fromUtf8(parseError != nullptr ? parseError->message : "unknown"));
        g_clear_error(&parseError);
        Logger::error(text);
        emit errorOccurred(text);
        return false;
    }
    g_clear_error(&parseError);
    pipeline_ = pipeline;

    displaySink_ = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), "displaysink"));
    if (aiEnabled_) {
        aiSink_ = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), "aisink"));
    }

    // appsink 回调（GStreamer 流线程 -> 无锁入队，无 UI 交互）
    GstAppSinkCallbacks displayCallbacks = {};
    displayCallbacks.new_sample = &GStreamerPipeline::onDisplaySample;
    gst_app_sink_set_callbacks(displaySink_, &displayCallbacks, this, nullptr);
    if (aiSink_ != nullptr) {
        GstAppSinkCallbacks aiCallbacks = {};
        aiCallbacks.new_sample = &GStreamerPipeline::onAiSample;
        gst_app_sink_set_callbacks(aiSink_, &aiCallbacks, this, nullptr);
    }

    // 总线：同步回调中仅做引用计数搬运，排队到主线程解析（UI 隔离红线）
    GstBus* bus = gst_element_get_bus(pipeline_);
    gst_bus_set_sync_handler(bus, &GStreamerPipeline::busSyncCallback, this, nullptr);
    gst_object_unref(bus);

    const GstStateChangeReturn rc = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (rc == GST_STATE_CHANGE_FAILURE) {
        Logger::error(QString::fromLocal8Bit("视频：管线置 PLAYING 失败"));
        stop();
        emit errorOccurred(QString::fromLocal8Bit("管线启动失败"));
        return false;
    }

    lastFrameMs_.store(0, std::memory_order_release);
    statLastTotal_ = totalFrames_.load(std::memory_order_acquire);
    statLastMs_ = QDateTime::currentMSecsSinceEpoch();
    startedAtMs_ = statLastMs_;
    noPacketHintLogged_ = false;
    watchdog_.start();
    running_.store(true, std::memory_order_release);
    emit pipelineStateChanged(true);

    Logger::info(QString::fromLocal8Bit("视频：管线已启动（%1:%2，抖动 %3ms，解码 %4，AI 分支 %5）")
                     .arg(bindAddress_.isEmpty() ? QStringLiteral("0.0.0.0") : bindAddress_)
                     .arg(port_)
                     .arg(jitterLatencyMs_)
                     .arg(decoder)
                     .arg(aiEnabled_ ? QString::fromLatin1("ON") : QString::fromLatin1("OFF")));
    return true;
}

void GStreamerPipeline::stopForExit()
{
    if (pipeline_ == nullptr) {
        return;
    }
    // TD-8：带流状态下销毁（NULL+unref）或强杀驱动工作线程都会触发
    // Intel ICD 崩溃；退出路径仅停止数据流（READY），保留管线与
    // d3d11 设备存活至进程结束，由内核回收
    GstBus* bus = gst_element_get_bus(pipeline_);
    if (bus != nullptr) {
        gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
        gst_object_unref(bus);
    }
    gst_element_set_state(pipeline_, GST_STATE_READY);
    watchdog_.stop();
    running_.store(false, std::memory_order_release);
    DataManager::instance().setVideoActive(false);
    pipeline_ = nullptr;      // 故意泄漏 GstPipeline（不 unref）
    displaySink_ = nullptr;
    aiSink_ = nullptr;
    Logger::info(QString::fromLocal8Bit("视频：退出停流（管线保留至进程结束）"));
}

void GStreamerPipeline::scheduleRestart(int delayMs)
{
    if (restartScheduled_) {
        return; // 去重，防错误风暴引发重建风暴
    }
    restartScheduled_ = true;
    QTimer::singleShot(delayMs, this, [this] {
        restartScheduled_ = false;
        if (isRunning() || pipeline_ != nullptr) {
            Logger::info(QString::fromLocal8Bit("视频：自愈重建管线"));
            stop();
            buildAndPlay();
        }
    });
}

void GStreamerPipeline::onWatchdog()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = lastFrameMs_.load(std::memory_order_acquire);

    // 无帧自愈（自愈红线；阈值/延迟来自 [video]）
    const AppConfig& cfg = AppConfig::instance();
    if (running_.load(std::memory_order_acquire) && last > 0
        && (now - last) > cfg.videoStallThresholdMs()) {
        Logger::warning(QString::fromLocal8Bit("视频：长时间未收到帧，触发自愈重建"));
        DataManager::instance().setVideoActive(false);
        scheduleRestart(cfg.videoRestartDelayMs());
        return;
    }

    // 从未收到包：重建无意义（包根本没进来），给一次性的对接排查提示
    if (running_.load(std::memory_order_acquire) && last == 0 && startedAtMs_ > 0
        && (now - startedAtMs_) > cfg.videoNoPacketHintMs() && !noPacketHintLogged_) {
        noPacketHintLogged_ = true;
        Logger::warning(QString::fromLocal8Bit("视频：启动 %1ms 未收到任何 RTP 包。排查："
                                               "① Windows 防火墙需放行本程序入站 UDP（端口 %2）；"
                                               "② 板端 CamStream 推流目标应为 %3:%2（示例：camstream_1080p /dev/video1 %3 %2 4000）；"
                                               "③ ini [network] host_ip 需与本机网卡实际地址一致（当前 %3，留空=全接口）")
                            .arg(now - startedAtMs_)
                            .arg(port_)
                            .arg(bindAddress_.isEmpty() ? QStringLiteral("192.168.137.1") : bindAddress_));
    }

    // 周期统计上报（fps/丢帧/最近帧时刻，[video] stats_interval_ms）
    if ((statLastMs_ == 0) || ((now - statLastMs_) >= cfg.videoStatsIntervalMs())) {
        const quint64 total = totalFrames_.load(std::memory_order_acquire);
        VideoStats stats;
        stats.totalFrames = total;
        stats.droppedFrames = displayDropped_.load(std::memory_order_acquire);
        stats.lastFrameTimeMs = last;
        stats.fps = (statLastMs_ > 0)
                ? (static_cast<double>(total - statLastTotal_) * 1000.0
                   / static_cast<double>(now - statLastMs_))
                : 0.0;
        DataManager::instance().setVideoStats(stats);
        statLastTotal_ = total;
        statLastMs_ = now;

        // AI 帧环溢出单独观察（推理消费不及/引擎未就绪），不与显示丢帧混计
        const quint64 aiDropped = aiDropped_.load(std::memory_order_acquire);
        if (aiDropped > statLastAiDropped_) {
            Logger::warning(QString::fromLocal8Bit("视频：AI 帧环溢出丢帧 +%1（累计 %2，推理消费不及或引擎未就绪）")
                                .arg(aiDropped - statLastAiDropped_)
                                .arg(aiDropped));
            statLastAiDropped_ = aiDropped;
        }

        if (running_.load(std::memory_order_acquire) && last > 0
            && (now - last) <= 2000) {
            DataManager::instance().setVideoActive(true); // 原子去重，无信号风暴
        }
    }
}

// ---------------------------------------------------------------- callbacks

GstBusSyncReply GStreamerPipeline::busSyncCallback(GstBus* /*bus*/, GstMessage* msg,
                                                   gpointer self)
{
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR:
    case GST_MESSAGE_WARNING:
    case GST_MESSAGE_EOS: {
        // GStreamer 内部线程 -> 主线程排队（跨线程投递红线）
        auto* pipe = static_cast<GStreamerPipeline*>(self);
        gst_message_ref(msg);
        QMetaObject::invokeMethod(pipe, [pipe, msg] { pipe->handleBusMessage(msg); },
                                  Qt::QueuedConnection);
        break;
    }
    default:
        break;
    }
    return GST_BUS_DROP;
}

void GStreamerPipeline::handleBusMessage(GstMessage* msg)
{
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(msg, &err, &debug);
        const QString text = QString::fromLocal8Bit("视频：GStreamer 错误 %1 [%2]")
                                 .arg(QString::fromUtf8(err != nullptr ? err->message : "unknown"),
                                      QString::fromUtf8(debug != nullptr ? debug : ""));
        g_clear_error(&err);
        g_free(debug);
        Logger::error(text);
        emit errorOccurred(text);
        DataManager::instance().setVideoActive(false);
        scheduleRestart(AppConfig::instance().videoBusRestartDelayMs());
        break;
    }
    case GST_MESSAGE_EOS:
        Logger::warning(QString::fromLocal8Bit("视频：收到 EOS，自愈重建"));
        DataManager::instance().setVideoActive(false);
        scheduleRestart(AppConfig::instance().videoBusRestartDelayMs());
        break;
    case GST_MESSAGE_WARNING:
    default:
        break;
    }
    gst_message_unref(msg);
}

std::shared_ptr<VideoFrame> GStreamerPipeline::pullSample(GstAppSink* sink)
{
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return nullptr;
    }
    std::shared_ptr<VideoFrame> frame;
    GstCaps* caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    if (caps != nullptr && gst_video_info_from_caps(&info, caps)) {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (buffer != nullptr && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            const int width = GST_VIDEO_INFO_WIDTH(&info);
            const int height = GST_VIDEO_INFO_HEIGHT(&info);
            const int bpp = GST_VIDEO_INFO_COMP_PSTRIDE(&info, 0);
            const int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);

            frame = std::make_shared<VideoFrame>();
            frame->width = width;
            frame->height = height;
            frame->bytesPerPixel = bpp;
            frame->timestampNs = static_cast<std::int64_t>(GST_BUFFER_PTS(buffer));
            frame->data.resize(static_cast<std::size_t>(width) * height * bpp);

            // 逐行去 stride，紧排列化（BGRA/RGB 单平面打包格式）
            const std::uint8_t* src = map.data;
            std::uint8_t* dst = frame->data.data();
            for (int y = 0; y < height; ++y) {
                std::memcpy(dst + static_cast<std::size_t>(y) * width * bpp,
                            src + static_cast<std::size_t>(y) * stride,
                            static_cast<std::size_t>(width) * bpp);
            }
            gst_buffer_unmap(buffer, &map);
        }
    }
    gst_sample_unref(sample);
    return frame;
}

void GStreamerPipeline::pullFrameToRing(GstAppSink* sink,
                                        RingBuffer<VideoFrame, 4>& ring,
                                        std::atomic<quint64>& dropped)
{
    std::shared_ptr<VideoFrame> frame = pullSample(sink);
    if (frame == nullptr) {
        return;
    }
    if (!ring.push(std::move(*frame))) {
        dropped.fetch_add(1, std::memory_order_acq_rel); // 环满丢弃（低延迟优先）
    }
}

void GStreamerPipeline::pullFrameToHub(GstAppSink* sink, VideoFrameHub& hub,
                                       std::atomic<quint64>& index)
{
    std::shared_ptr<VideoFrame> frame = pullSample(sink);
    if (frame == nullptr) {
        return;
    }
    // 发布最新帧（覆盖旧帧不积压；序号单调递增且 > 0，供视图判断新帧）
    const quint64 seq = index.fetch_add(1, std::memory_order_acq_rel) + 1U;
    hub.publish(std::shared_ptr<const VideoFrame>(std::move(frame)), seq);
}

GstFlowReturn GStreamerPipeline::onDisplaySample(GstAppSink* sink, gpointer self)
{
    auto* pipe = static_cast<GStreamerPipeline*>(self);
    pipe->pullFrameToHub(sink, pipe->displayHub_, pipe->displayFrameIndex_);
    pipe->lastFrameMs_.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_release);
    pipe->totalFrames_.fetch_add(1, std::memory_order_acq_rel);
    return GST_FLOW_OK;
}

GstFlowReturn GStreamerPipeline::onAiSample(GstAppSink* sink, gpointer self)
{
    auto* pipe = static_cast<GStreamerPipeline*>(self);
    pipe->pullFrameToRing(sink, pipe->aiFrames_, pipe->aiDropped_);
    return GST_FLOW_OK;
}

} // namespace salacia
