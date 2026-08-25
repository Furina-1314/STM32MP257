#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "IModelInfer.h"
#include "utils/RingBuffer.h"
#include "video/VideoFrame.h"

class QThread;
class QTimer;

namespace Ort {
class Env;
class Session;
class SessionOptions;
class MemoryInfo;
} // namespace Ort

namespace salacia {

// ONNX Runtime 异构推理引擎（Worker-Object）
//
// 线程模型（多线程规范红线）：
//  - start()/stop() 仅主线程调用；内部创建专属推理线程并 moveToThread，
//    模型加载（可能数秒）完全脱离 GUI 线程；
//  - 推理循环由工作线程内 5ms QTimer 驱动：排空帧环取最新 ->
//    preprocess(NCHW) -> Run -> postprocess(阈值+NMS) ->
//    DataManager::setDetections（读写锁发布，UI 信号安全）；
//  - 逆序退出：stop() 先经 BlockingQueuedConnection 进入工作线程停止
//    定时器并释放 ONNX/GPU 上下文（GPU 资源在工作线程销毁），
//    再 quit()/wait() 线程。
//
// 动态 EP 探测（异构算力红线）：auto 模式按 CUDA -> TensorRT ->
// DirectML -> OpenVINO -> CPU 优先级，基于 Ort::GetAvailableProviders()
// 探测结果选择，显式配置值不可用时回退 CPU 并告警；绑定结果经
// backendReady 信号（排队）广播并写日志。
class OnnxInferEngine : public QObject, public IModelInfer
{
    Q_OBJECT

public:
    explicit OnnxInferEngine(QObject* parent = nullptr);
    ~OnnxInferEngine() override;
    Q_DISABLE_COPY(OnnxInferEngine)

    // 主线程调用：启动推理线程并异步初始化（frameSource=GStreamerPipeline::aiFrames）
    void start(RingBuffer<VideoFrame, 4>* frameSource);
    // 幂等停止（逆序安全退出）
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    bool isReady() const { return ready_.load(std::memory_order_acquire); }
    int lastInferenceMs() const { return inferenceMs_.load(std::memory_order_acquire); }

    // --- IModelInfer（仅推理工作线程内调用） ---
    bool initialize() override;
    std::vector<Detection> infer(const VideoFrame& frame) override;
    QString backendName() const override { return backendName_; }
    int inputWidth() const override { return inputWidth_; }
    int inputHeight() const override { return inputHeight_; }

signals:
    // 均自主线程经排队连接发出
    void backendReady(const QString& backend);
    void engineFailed(const QString& reason);

private slots:
    void initOnWorker();    // 线程启动：初始化模型 + 启动轮询定时器
    void cleanupOnWorker(); // 线程停止前：停定时器 + 释放 ONNX/GPU 上下文
    void pollFrames();      // 推理主循环（5ms 节拍，仅有帧才推理）

private:
    // 返回实际绑定的后端名；失败返回空串（内部已尝试 CPU 回退）
    QString appendExecutionProvider(Ort::SessionOptions& options);

    RingBuffer<VideoFrame, 4>* source_ = nullptr; // 不拥有（GStreamerPipeline 所有）

    std::unique_ptr<QThread> worker_;
    QTimer* pollTimer_ = nullptr; // 工作线程内创建/销毁（不设父对象）

    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<Ort::MemoryInfo> memoryInfo_;
    std::string inputName_;
    std::string outputName_;
    std::vector<float> inputBuffer_; // 复用的 NCHW 输入缓冲

    int inputWidth_ = 0;
    int inputHeight_ = 0;
    QString backendName_;
    float confThreshold_ = 0.5F;
    float nmsIou_ = 0.45F;

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<int> inferenceMs_{0};

    VideoFrame pending_;      // 工作线程私有：最新待推理帧
    bool sizeWarned_ = false; // 尺寸不匹配告警只发一次
};

} // namespace salacia
