// ORT 头会间接引入 Windows 头：禁用 min/max 宏与精简 Win32 面
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "OnnxInferEngine.h"

#include <QFile>
#include <QThread>
#include <QTimer>

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "core/AppConfig.h"
#include "core/DataManager.h"
#include "core/Logger.h"

namespace salacia {

namespace {

// 候选框（后处理中间态）
struct Candidate
{
    float x1 = 0.0F; // 归一化坐标
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    float score = 0.0F;
    int classId = -1;
};

float boxIou(const Candidate& a, const Candidate& b)
{
    const float interW = std::max(0.0F, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
    const float interH = std::max(0.0F, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
    const float inter = interW * interH;
    const float areaA = (a.x2 - a.x1) * (a.y2 - a.y1);
    const float areaB = (b.x2 - b.x1) * (b.y2 - b.y1);
    const float sum = areaA + areaB - inter;
    return (sum > 0.0F) ? (inter / sum) : 0.0F;
}

std::vector<Detection> nonMaxSuppression(std::vector<Candidate>& candidates, float iouThreshold)
{
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    std::vector<Detection> result;
    for (const Candidate& c : candidates) {
        bool suppressed = false;
        for (const Detection& kept : result) {
            Candidate k;
            k.x1 = kept.x;
            k.y1 = kept.y;
            k.x2 = kept.x + kept.w;
            k.y2 = kept.y + kept.h;
            if ((c.classId == kept.classId) && (boxIou(c, k) > iouThreshold)) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) {
            Detection d;
            d.classId = c.classId;
            d.confidence = c.score;
            d.x = c.x1;
            d.y = c.y1;
            d.w = c.x2 - c.x1;
            d.h = c.y2 - c.y1;
            result.push_back(d);
        }
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------- 生命周期

OnnxInferEngine::OnnxInferEngine(QObject* parent)
    : QObject(parent)
{
}

OnnxInferEngine::~OnnxInferEngine()
{
    stop(); // RAII 兜底（幂等）
}

void OnnxInferEngine::start(RingBuffer<VideoFrame, 4>* frameSource)
{
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return; // 已在运行
    }
    source_ = frameSource;

    worker_ = std::make_unique<QThread>();
    worker_->setObjectName(QStringLiteral("salacia-ai"));
    moveToThread(worker_.get());
    connect(worker_.get(), &QThread::started, this, &OnnxInferEngine::initOnWorker);
    worker_->start();
    Logger::info(QString::fromLocal8Bit("AI：推理线程已启动，开始异步加载模型"));
}

void OnnxInferEngine::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (worker_ != nullptr) {
        // 工作线程自终结：停定时器 -> 释放 ONNX/GPU 上下文 -> 退出事件循环。
        // 严禁 BlockingQueuedConnection（对象若滞留调用线程即死锁），
        // 主线程仅做限时阶梯等待，GUI 永不永久阻塞（安全退出红线）
        QMetaObject::invokeMethod(this, &OnnxInferEngine::shutdownOnWorker,
                                  Qt::QueuedConnection);
        if (!worker_->wait(3000)) {
            Logger::error(QString::fromLocal8Bit("AI：停止超时，请求线程中断"));
            worker_->requestInterruption();
            if (!worker_->wait(2000)) {
                Logger::error(QString::fromLocal8Bit("AI：线程未响应中断，强制终止"));
                worker_->terminate();
                worker_->wait(1000);
            }
        }
    }
    // 兜底释放（幂等；正常路径已在工作线程完成）
    session_.reset();
    memoryInfo_.reset();
    env_.reset();
    ready_.store(false, std::memory_order_release);
    Logger::info(QString::fromLocal8Bit("AI：推理引擎已停止，上下文已释放"));
}

void OnnxInferEngine::initOnWorker()
{
    const AppConfig& cfg = AppConfig::instance();
    confThreshold_ = static_cast<float>(cfg.confidenceThreshold());
    nmsIou_ = static_cast<float>(cfg.nmsIouThreshold());

    // 排空定时器必须无条件启动（含加载失败）：pollFrames 内部有 ready_ 门控，
    // 失败时仅持续丢弃帧环中的积压帧。若失败即不起定时器，AI 帧环（容量 4）
    // 将永远无人消费 -> 生产端每帧环满丢弃，状态栏丢帧数满速率上涨
    //（真机对接期"全部丢帧"假象的根因）
    pollTimer_ = new QTimer(); // 工作线程内创建（不设父，cleanup 中删除）
    pollTimer_->setInterval(5);
    connect(pollTimer_, &QTimer::timeout, this, &OnnxInferEngine::pollFrames);
    pollTimer_->start();

    if (!initialize()) {
        ready_.store(false, std::memory_order_release);
        return; // engineFailed 已在 initialize 内发出；线程排空待 stop()
    }

    ready_.store(true, std::memory_order_release);
    Logger::info(QString::fromLocal8Bit("AI：就绪（%1，输入 %2x%3，阈值 %4，NMS %5）")
                     .arg(backendName_)
                     .arg(inputWidth_)
                     .arg(inputHeight_)
                     .arg(confThreshold_)
                     .arg(nmsIou_));
}

void OnnxInferEngine::shutdownOnWorker()
{
    if (pollTimer_ != nullptr) {
        pollTimer_->stop();
        delete pollTimer_;
        pollTimer_ = nullptr;
    }
    // ONNX/GPU 上下文在其创建线程（本工作线程）销毁
    session_.reset();
    memoryInfo_.reset();
    env_.reset();
    thread()->quit(); // 自终结事件循环，主线程限时 wait 收割
}

// ---------------------------------------------------------------- 初始化

QString OnnxInferEngine::appendExecutionProvider(Ort::SessionOptions& options)
{
    const QString wanted = AppConfig::instance().executionProvider();

    std::vector<std::string> available;
    {
        const auto providers = Ort::GetAvailableProviders();
        available.assign(providers.begin(), providers.end());
    }
    const auto has = [&available](const char* name) {
        return std::any_of(available.begin(), available.end(),
                           [name](const std::string& p) { return p == name; });
    };

    // 探测优先级：CUDA -> TensorRT -> DirectML -> OpenVINO -> CPU（异构红线）
    QString choice = QStringLiteral("CPU");
    if (wanted == QStringLiteral("auto")) {
        if (has("CUDAExecutionProvider")) {
            choice = QStringLiteral("CUDA");
        } else if (has("TensorrtExecutionProvider")) {
            choice = QStringLiteral("TensorRT");
        } else if (has("DmlExecutionProvider")) {
            choice = QStringLiteral("DirectML");
        } else if (has("OpenVINOExecutionProvider")) {
            choice = QStringLiteral("OpenVINO");
        }
    } else {
        const QString mapped = (wanted == QStringLiteral("dml")) ? QStringLiteral("DirectML")
                               : (wanted == QStringLiteral("cuda")) ? QStringLiteral("CUDA")
                               : (wanted == QStringLiteral("tensorrt")) ? QStringLiteral("TensorRT")
                               : (wanted == QStringLiteral("openvino")) ? QStringLiteral("OpenVINO")
                               : QStringLiteral("CPU");
        choice = mapped;
    }

    try {
        if (choice == QStringLiteral("CUDA") && has("CUDAExecutionProvider")) {
            OrtCUDAProviderOptions cudaOptions = {};
            options.AppendExecutionProvider_CUDA(cudaOptions);
        } else if (choice == QStringLiteral("TensorRT") && has("TensorrtExecutionProvider")) {
            OrtTensorRTProviderOptions trtOptions = {};
            options.AppendExecutionProvider_TensorRT(trtOptions);
        } else if (choice == QStringLiteral("DirectML") && has("DmlExecutionProvider")) {
            // DML 无 cxx 封装，走 C API（Ort::SessionOptions 隐式转裸指针）
            OrtStatus* st = OrtSessionOptionsAppendExecutionProvider_DML(options, 0);
            if (st != nullptr) {
                const char* msg = Ort::GetApi().GetErrorMessage(st);
                Ort::GetApi().ReleaseStatus(st);
                throw Ort::Exception(std::string(msg), OrtErrorCode::ORT_EP_FAIL);
            }
        } else if (choice == QStringLiteral("OpenVINO") && has("OpenVINOExecutionProvider")) {
            OrtOpenVINOProviderOptions ovinoOptions = {};
            options.AppendExecutionProvider_OpenVINO(ovinoOptions);
        }
        // 未命中任何分支即为 CPU
    } catch (const Ort::Exception& e) {
        Logger::warning(QString::fromLocal8Bit("AI：绑定 %1 失败（%2），回退 CPU")
                            .arg(choice, QString::fromUtf8(e.what())));
        choice = QStringLiteral("CPU");
    }
    return choice;
}

bool OnnxInferEngine::initialize()
{
    const QString modelPath = AppConfig::instance().modelPath();

    if (modelPath.isEmpty() || !QFile::exists(modelPath)) {
        const QString reason = QString::fromLocal8Bit("模型文件不存在：%1").arg(modelPath);
        Logger::error(QString::fromLocal8Bit("AI：") + reason);
        emit engineFailed(reason);
        return false;
    }

    try {
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "salacia-ai");
        memoryInfo_ = std::make_unique<Ort::MemoryInfo>(Ort::MemoryInfo::CreateCpu(
                OrtArenaAllocator, OrtMemTypeDefault));

        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        backendName_ = appendExecutionProvider(options);

        session_ = std::make_unique<Ort::Session>(
                *env_, modelPath.toStdWString().c_str(), options);

        // 输入/输出名与静态形状（网络输入边界校验）
        {
            Ort::AllocatorWithDefaultOptions allocator;
            auto inName = session_->GetInputNameAllocated(0, allocator);
            auto outName = session_->GetOutputNameAllocated(0, allocator);
            inputName_ = (inName != nullptr) ? inName.get() : "";
            outputName_ = (outName != nullptr) ? outName.get() : "";
        }

        const auto inputShape = session_->GetInputTypeInfo(0)
                                      .GetTensorTypeAndShapeInfo()
                                      .GetShape();
        if (inputShape.size() == 4) {
            // NCHW：动态维度（0 或 -1）时回退配置值
            inputWidth_ = (inputShape[3] > 0) ? static_cast<int>(inputShape[3])
                                              : AppConfig::instance().inputWidth();
            inputHeight_ = (inputShape[2] > 0) ? static_cast<int>(inputShape[2])
                                               : AppConfig::instance().inputHeight();
        } else {
            inputWidth_ = AppConfig::instance().inputWidth();
            inputHeight_ = AppConfig::instance().inputHeight();
        }

        Logger::info(QString::fromLocal8Bit("AI：模型已加载（%1，绑定后端 %2）")
                         .arg(modelPath, backendName_));
        emit backendReady(backendName_);
        return true;
    } catch (const Ort::Exception& e) {
        session_.reset();
        memoryInfo_.reset();
        env_.reset();
        const QString reason = QString::fromLocal8Bit("ONNX Runtime 异常：%1")
                                   .arg(QString::fromUtf8(e.what()));
        Logger::error(QString::fromLocal8Bit("AI：") + reason);
        emit engineFailed(reason);
        return false;
    }
}

// ---------------------------------------------------------------- 推理

void OnnxInferEngine::pollFrames()
{
    bool got = false;
    while ((source_ != nullptr) && source_->pop(pending_)) {
        got = true; // 排空取最新（推理慢时丢弃旧帧，保实时）
    }
    if (!got || !ready_.load(std::memory_order_acquire)) {
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<Detection> dets = infer(pending_);
    const auto t1 = std::chrono::steady_clock::now();
    inferenceMs_.store(
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()),
            std::memory_order_release);

    DataManager::instance().setDetections(dets); // 读写锁发布（信号自主排队）
}

std::vector<Detection> OnnxInferEngine::infer(const VideoFrame& frame)
{
    if (session_ == nullptr) {
        return {};
    }
    if ((frame.width != inputWidth_) || (frame.height != inputHeight_)
        || (frame.bytesPerPixel != 3)) {
        if (!sizeWarned_) {
            sizeWarned_ = true;
            Logger::warning(QString::fromLocal8Bit("AI：帧尺寸 %1x%2(x%3) 与模型输入 %4x%5 不符，已跳帧")
                                .arg(frame.width)
                                .arg(frame.height)
                                .arg(frame.bytesPerPixel)
                                .arg(inputWidth_)
                                .arg(inputHeight_));
        }
        return {};
    }

    // HWC RGB -> NCHW float，归一化 /255
    const std::size_t plane = static_cast<std::size_t>(frame.width) * frame.height;
    inputBuffer_.resize(plane * 3U);
    const std::uint8_t* src = frame.data.data();
    float* planeR = inputBuffer_.data();
    float* planeG = planeR + plane;
    float* planeB = planeG + plane;
    for (std::size_t i = 0; i < plane; ++i) {
        planeR[i] = static_cast<float>(src[i * 3U + 0U]) / 255.0F;
        planeG[i] = static_cast<float>(src[i * 3U + 1U]) / 255.0F;
        planeB[i] = static_cast<float>(src[i * 3U + 2U]) / 255.0F;
    }

    const std::int64_t shape[4] = {1, 3, inputHeight_, inputWidth_};
    auto tensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_, inputBuffer_.data(), inputBuffer_.size(), shape, 4);

    const char* inputNames[] = {inputName_.c_str()};
    const char* outputNames[] = {outputName_.c_str()};
    auto outputs = session_->Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1,
                                 outputNames, 1);
    if (outputs.empty() || !outputs.front().IsTensor()) {
        return {};
    }

    // 后处理：支持两种检测头布局
    const Ort::Value& out = outputs.front();
    const auto outShape = out.GetTensorTypeAndShapeInfo().GetShape();
    if (outShape.size() != 3) {
        Logger::warning(QString::fromLocal8Bit("AI：输出维度异常（%1 维），忽略本帧")
                            .arg(static_cast<int>(outShape.size())));
        return {};
    }
    const float* data = out.GetTensorData<float>();
    const std::size_t count = out.GetTensorTypeAndShapeInfo().GetElementCount();
    if (data == nullptr || count == 0U) {
        return {};
    }

    std::vector<Candidate> candidates;
    const float invW = 1.0F / static_cast<float>(inputWidth_);
    const float invH = 1.0F / static_cast<float>(inputHeight_);

    if (outShape[1] < outShape[2]) {
        // 布局 A（YOLOv8 风格）：[1, C, N]，C=4+numClasses，无 objectness
        const int channels = static_cast<int>(outShape[1]);
        const int num = static_cast<int>(outShape[2]);
        const int numClasses = channels - 4;
        if (numClasses <= 0) {
            return {};
        }
        for (int n = 0; n < num; ++n) {
            const float cx = data[static_cast<std::size_t>(n)];
            const float cy = data[static_cast<std::size_t>(num + n)];
            const float w = data[static_cast<std::size_t>(2 * num + n)];
            const float h = data[static_cast<std::size_t>(3 * num + n)];
            int bestClass = -1;
            float bestScore = -1.0F;
            for (int c = 0; c < numClasses; ++c) {
                const float s = data[static_cast<std::size_t>((4 + c) * num + n)];
                if (s > bestScore) {
                    bestScore = s;
                    bestClass = c;
                }
            }
            if ((bestClass >= 0) && (bestScore >= confThreshold_)) {
                Candidate cand;
                cand.x1 = (cx - w * 0.5F) * invW;
                cand.y1 = (cy - h * 0.5F) * invH;
                cand.x2 = (cx + w * 0.5F) * invW;
                cand.y2 = (cy + h * 0.5F) * invH;
                cand.score = bestScore;
                cand.classId = bestClass;
                candidates.push_back(cand);
            }
        }
    } else {
        // 布局 B（YOLOv5 风格）：[1, N, C]，C=5+numClasses（第 4 位 objectness）
        const int num = static_cast<int>(outShape[1]);
        const int channels = static_cast<int>(outShape[2]);
        const int numClasses = channels - 5;
        if (numClasses <= 0) {
            return {};
        }
        for (int n = 0; n < num; ++n) {
            const float* row = data + static_cast<std::size_t>(n) * channels;
            const float obj = row[4];
            if (obj < confThreshold_) {
                continue;
            }
            int bestClass = -1;
            float bestScore = -1.0F;
            for (int c = 0; c < numClasses; ++c) {
                const float s = row[5 + c];
                if (s > bestScore) {
                    bestScore = s;
                    bestClass = c;
                }
            }
            if ((bestClass >= 0) && (obj * bestScore >= confThreshold_)) {
                Candidate cand;
                cand.x1 = (row[0] - row[2] * 0.5F) * invW;
                cand.y1 = (row[1] - row[3] * 0.5F) * invH;
                cand.x2 = (row[0] + row[2] * 0.5F) * invW;
                cand.y2 = (row[1] + row[3] * 0.5F) * invH;
                cand.score = obj * bestScore;
                cand.classId = bestClass;
                candidates.push_back(cand);
            }
        }
    }

    return nonMaxSuppression(candidates, nmsIou_);
}

} // namespace salacia
