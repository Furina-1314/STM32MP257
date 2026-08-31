#pragma once

#include <QObject>
#include <QMetaType>
#include <QString>

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <vector>

namespace salacia {

// 舱体状态（20Hz 遥测；网络线程写 / UI 线程读）
// 板载传感器：MPU6500（姿态）+ 舱内温湿度 + 电池电压
struct RovState
{
    qint64 timestampMs = 0;                              // 采集时间戳
    float quaternion[4] = {1.0F, 0.0F, 0.0F, 0.0F};      // 姿态四元数 w x y z
    float rollDeg = 0.0F;                                // 欧拉角（度）
    float pitchDeg = 0.0F;
    float yawDeg = 0.0F;
    float cabinTempC = 0.0F;                             // 舱内温度（摄氏度）
    float cabinHumidityPct = 0.0F;                       // 舱内湿度（%RH）
    float batteryVoltage = 0.0F;                         // 电池电压（伏）
    float batteryPercent = 0.0F;                         // 电池电量（%，电压折算）
};

// 单个 AI 检测框（归一化坐标 0~1，相对推理输入帧）
struct Detection
{
    int classId = -1;
    float confidence = 0.0F;
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
};

// 视频链路统计（GStreamer 回调线程写 / UI 线程读）
struct VideoStats
{
    double fps = 0.0;                 // 实时帧率
    qint64 lastFrameTimeMs = 0;       // 最近一帧本地时刻（超时判活依据）
    quint64 droppedFrames = 0;        // 丢帧计数
    quint64 totalFrames = 0;          // 累计帧数
};

// 全局数据管理器（单例）
//
// 线程模型（多线程规范红线）：
//  - 结构化低频共享数据（姿态 20Hz / AI 结果数 Hz / 视频统计）：
//    std::shared_mutex 读写锁——工作线程 unique_lock 写、UI 线程
//    shared_lock 高频读；数据块 alignas(64) 缓存行对齐防伪共享；
//  - 标量链路状态（视频/遥测/SSH 在线）：std::atomic<bool>，无锁；
//  - 信号均为无载荷“变更通知”：UI 收到后自行调用 const 读取器取数，
//    避免自定义类型跨线程排队的元类型注册与拷贝；工作线程发信号安全，
//    UI 侧连接时必须显式 Qt::QueuedConnection；
//  - 生命周期：Meyers 单例于静态析构期销毁；退出时 MainWindow 必须
//    先停止全部工作线程（逆序安全退出），再离开 main()。
class DataManager : public QObject
{
    Q_OBJECT

public:
    static DataManager& instance();

    // ---- 写入（各工作线程） ----
    void setRovState(const RovState& state);                // 遥测线程 20Hz
    void setDetections(const std::vector<Detection>& items); // 推理线程
    void setClassNames(const std::vector<QString>& names);   // 推理线程，模型就绪后一次
    void setVideoStats(const VideoStats& stats);             // GStreamer 回调线程
    void setVideoActive(bool on);
    void setTelemetryActive(bool on);
    void setSshConnected(bool on);

    // ---- 读取（任意线程；UI 高频轮询或信号触发） ----
    RovState rovState() const;
    std::vector<Detection> detections() const;
    std::vector<QString> classNames() const;
    VideoStats videoStats() const;
    bool videoActive() const { return videoActive_.load(std::memory_order_acquire); }
    bool telemetryActive() const { return telemetryActive_.load(std::memory_order_acquire); }
    bool sshConnected() const { return sshConnected_.load(std::memory_order_acquire); }

signals:
    // 仅通知“有新数据”，无载荷；跨线程自动排队
    void rovStateUpdated();
    void detectionsUpdated();
    void videoStatsUpdated();
    void linkStateChanged();

private:
    explicit DataManager(QObject* parent = nullptr);
    ~DataManager() override;
    Q_DISABLE_COPY(DataManager)

    mutable std::shared_mutex rovStateMutex_;
    alignas(64) RovState rovState_;

    mutable std::shared_mutex detectionsMutex_;
    alignas(64) std::vector<Detection> detections_;
    alignas(64) std::vector<QString> classNames_; // 类别名表（索引 = classId）

    mutable std::shared_mutex videoStatsMutex_;
    alignas(64) VideoStats videoStats_;

    alignas(64) std::atomic<bool> videoActive_{false};
    alignas(64) std::atomic<bool> telemetryActive_{false};
    alignas(64) std::atomic<bool> sshConnected_{false};
};

} // namespace salacia

// 跨线程排队信号传递所需（SensorModel -> DataManager 桥接）
Q_DECLARE_METATYPE(salacia::RovState)
