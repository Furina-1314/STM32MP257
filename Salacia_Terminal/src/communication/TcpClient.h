#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "communication/FunctionRegistry.h"
#include "communication/WireCodec.h"

class QTcpSocket;
class QTimer;
class QThread;

namespace salacia {

// Windows<->A35 TCP 控制通道客户端（Worker-Object 红线）
//
// 线程模型：
//  - 本对象驻留主线程承载控制面；socket/定时器全部在专属工作线程；
//  - sendFrame 可在任意线程调用（互斥队列投递，工作线程 5ms 节拍冲刷）；
//  - 帧解析与 ACK 匹配均在工作线程完成，结果经信号（自动排队）投递主线程；
//  - stop() 为"工作线程自终结 + 主线程限时阶梯"（禁 BlockingQueuedConnection）。
//
// 发送策略（低延迟红线）：
//  - 双优先级队列：estop/emergency（priority < kPriorityNormal）插队，永不因
//    队列溢出被丢弃；普通控制溢出丢最旧；
//  - seq 在实际出队编码时分配（单调递增，0-65535 回绕）；
//  - 需 ACK 的请求进入挂起表，超时发 requestTimedOut；迟到响应只计数丢弃；
//  - 断开瞬间清空发送队列与挂起表（不重放危险指令红线）；重连成功后自动
//    发送 ask/status/sensor all 只读权威状态查询。
class TcpClient : public QObject
{
    Q_OBJECT

public:
    struct Settings
    {
        QString host;
        quint16 port = 7000U;
        int connectTimeoutMs = 3000;
        int requestTimeoutMs = 1000;
        bool heartbeatEnabled = true;
        int heartbeatIntervalMs = 1000;
        bool reconnectEnabled = true;
        int reconnectBaseMs = 1000;
        int reconnectMaxMs = 10000;
        int maxRetry = 0; // 0 = 无限
        bool noDelay = true;
        int recvBufferLimit = 65536;
        int maxPayload = 4096;
        int sendQueueCapacity = 64;
    };

    explicit TcpClient(QObject* parent = nullptr);
    ~TcpClient() override;

    void start(const Settings& settings);
    void stop(); // 幂等

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    bool isConnected() const { return connected_.load(std::memory_order_acquire); }

    // 任意线程：入队待发帧（funcId 须已在注册表登记；needsAck/priority 查表）。
    // seq 在出队编码时分配，分配后经 requestSent(seq, funcId) 通知调用方
    void sendFrame(quint16 funcId, const QByteArray& payload);

signals:
    void connectionStateChanged(bool connected);
    // 强类型业务帧（解码在工作线程按注册表完成，UI/模型不解析字节流）
    void sensorSummaryReady(const salacia::wire::SensorSummary& summary);
    void ackReceived(quint16 seq, quint16 errCode, quint16 funcId);
    void eventReceived(quint16 funcId, const QByteArray& payload);
    // 请求生命周期（payload 随附：调用方回填通道等上下文）
    void requestSent(quint16 seq, quint16 funcId, const QByteArray& payload);
    void requestTimedOut(quint16 seq, quint16 funcId);
    void lateAckDropped(quint16 seq);
    void clientError(const QString& message);

private slots:
    void initOnWorker();
    void shutdownOnWorker();
    void onFlushTick();
    void onSweepTick();
    void onHeartbeatTick();
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketReadyRead();
    void onSocketError();
    void tryReconnect();

private:
    struct PendingAck
    {
        quint16 funcId = 0U;
        qint64 sentAtMs = 0;
    };

    void enqueueLocked(quint16 funcId, const QByteArray& payload);
    void writeFrameNow(quint16 funcId, quint8 flags, const QByteArray& payload);
    void clearQueuesLocked();
    void scheduleReconnectLocked();
    void handleFrame(const wire::WireFrame& frame);

    Settings settings_;

    // ---- 任意线程访问（互斥）----
    std::mutex queueMutex_;
    std::vector<wire::WireFrame> urgentQueue_;  // estop/emergency（有界，永不丢）
    std::vector<wire::WireFrame> normalQueue_;  // 普通控制（有界，溢出丢最旧）

    // ---- 工作线程私有 ----
    QTcpSocket* socket_ = nullptr;
    std::unique_ptr<QThread> worker_;
    std::unique_ptr<wire::FrameAccumulator> accumulator_;
    QTimer* flushTimer_ = nullptr;
    QTimer* sweepTimer_ = nullptr;
    QTimer* heartbeatTimer_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;
    QTimer* connectWatchdog_ = nullptr;
    std::map<quint16, PendingAck> pendingAcks_;
    quint16 nextSeq_ = 0U;
    int retryCount_ = 0;
    int backoffMs_ = 0;
    quint64 unknownFuncCount_ = 0U;
    quint64 lateAckCount_ = 0U;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> everConnected_{false}; // 首连后为真：断开后普通指令拒绝入队
    int announcedState_ = -1;                // 最近广播的连接态（-1=未广播过；
                                              // 使首连尝试失败也发出"离线"）
};

} // namespace salacia
