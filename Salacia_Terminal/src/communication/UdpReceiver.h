#pragma once

#include <QObject>

#include <atomic>
#include <memory>

#include "sensor/MPU6500Processor.h"

class QThread;
class QUdpSocket;
class QTimer;

namespace salacia {

// 20Hz 遥测接收器（Worker-Object，事件驱动异步——I/O 线程红线）
//
// 线程模型：
//  - start()/stop() 仅主线程调用；内部创建专属遥测线程并 moveToThread；
//  - QUdpSocket 在工作线程内创建绑定，readyRead 事件驱动收包
//    （禁 waitForReadyRead 阻塞红线）；
//  - 收包路径：读取数据报 -> parseTelemetryPacket 边界校验（长度/魔数/
//    版本/CRC16）-> MPU6500Processor 姿态解算（同线程直调）->
//    DataManager::setAttitude（读写锁发布，信号自动排队）；
//  - 看门狗（工作线程 500ms）：1 秒无有效包 -> telemetryActive=false。
class UdpReceiver : public QObject
{
    Q_OBJECT

public:
    explicit UdpReceiver(QObject* parent = nullptr);
    ~UdpReceiver() override;
    Q_DISABLE_COPY(UdpReceiver)

    // 主线程调用：启动接收（port 来自 AppConfig [rov] telemetry_port）
    void start();
    // 幂等停止：工作线程内关 socket -> 退线程
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // 诊断计数（近似值）
    quint64 packetsReceived() const { return received_.load(std::memory_order_acquire); }
    quint64 packetsDropped() const { return dropped_.load(std::memory_order_acquire); }

signals:
    // 均自主线程经排队连接发出
    void telemetryActiveChanged(bool active);
    void receiverError(const QString& message);

private slots:
    void initOnWorker();   // 线程启动：socket 绑定 + 看门狗
    void cleanupOnWorker();
    void readPending();    // readyRead：批量取尽数据报
    void checkWatchdog();

private:
    std::unique_ptr<QThread> worker_;
    QUdpSocket* socket_ = nullptr; // 工作线程内创建/销毁（父对象 this）
    QTimer* watchdog_ = nullptr;   // 同上

    MPU6500Processor processor_;   // 遥测线程串行调用，无需加锁

    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::atomic<qint64> lastPacketMs_{0};
    std::atomic<quint64> received_{0};
    std::atomic<quint64> dropped_{0};
};

} // namespace salacia
