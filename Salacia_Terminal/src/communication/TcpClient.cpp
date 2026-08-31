#include "TcpClient.h"

#include <QDateTime>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/AppConfig.h"
#include "core/Logger.h"

namespace salacia {

namespace {
constexpr int kFlushTickMs = 5; // 发送队列冲刷节拍（覆盖 ≤50ms 指令红线）
} // namespace

TcpClient::TcpClient(QObject* parent)
    : QObject(parent)
{
}

TcpClient::~TcpClient()
{
    stop(); // RAII 兜底（幂等）
}

void TcpClient::start(const Settings& settings)
{
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return; // 已在运行
    }
    settings_ = settings;
    everConnected_.store(false, std::memory_order_release);
    {
        const std::lock_guard<std::mutex> lock(queueMutex_);
        urgentQueue_.clear();
        normalQueue_.clear();
    }

    qRegisterMetaType<wire::SensorSummary>("salacia::wire::SensorSummary");

    worker_ = std::make_unique<QThread>();
    worker_->setObjectName(QStringLiteral("salacia-tcp"));
    moveToThread(worker_.get());
    connect(worker_.get(), &QThread::started, this, &TcpClient::initOnWorker);
    worker_->start();
    Logger::info(QString::fromLocal8Bit("TCP：控制线程已启动（目标 %1:%2）")
                     .arg(settings_.host)
                     .arg(settings_.port));
}

void TcpClient::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // 工作线程自终结（禁 BlockingQueuedConnection），主线程限时阶梯收割
    QMetaObject::invokeMethod(this, &TcpClient::shutdownOnWorker,
                              Qt::QueuedConnection);
    if (worker_ != nullptr) {
        const AppConfig& cfg = AppConfig::instance();
        if (!worker_->wait(cfg.workerStopWaitMs())) {
            Logger::error(QString::fromLocal8Bit("TCP：停止超时，请求线程中断"));
            worker_->requestInterruption();
            if (!worker_->wait(cfg.workerInterruptWaitMs())) {
                Logger::error(QString::fromLocal8Bit("TCP：线程未响应中断，强制终止"));
                worker_->terminate();
                worker_->wait(cfg.workerTerminateWaitMs());
            }
        }
        worker_.reset();
    }
    connected_.store(false, std::memory_order_release);
    Logger::info(QString::fromLocal8Bit("TCP：控制线程已停止"));
}

// ---------------------------------------------------------------- 任意线程 API

void TcpClient::sendFrame(quint16 funcId, const QByteArray& payload)
{
    const wire::FunctionEntry* entry = wire::FunctionRegistry::findByFuncId(funcId);
    if (entry == nullptr) {
        emit clientError(QString::fromLocal8Bit("TCP：拒绝发送未注册函数 0x%1")
                             .arg(funcId, 4, 16, QLatin1Char('0')));
        return;
    }
    // 断开红线：首连建立后掉线期间，普通与紧急指令一律拒绝入队
    //（防重连后重放过期动作；首次连接建立前允许排队）
    if (everConnected_.load(std::memory_order_acquire)
        && !connected_.load(std::memory_order_acquire)) {
        emit clientError(QString::fromLocal8Bit("TCP：链路断开，指令被丢弃（%1）")
                             .arg(QString::fromLatin1(entry->name)));
        return;
    }
    if (static_cast<int>(payload.size()) > settings_.maxPayload) {
        emit clientError(QString::fromLocal8Bit("TCP：载荷超长（%1 > %2），拒绝发送 %3")
                             .arg(payload.size())
                             .arg(settings_.maxPayload)
                             .arg(QString::fromLatin1(entry->name)));
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(queueMutex_);
        wire::WireFrame frame;
        frame.funcId = funcId;
        frame.flags = entry->needsAck ? wire::kFlagNeedAck : 0U;
        frame.payload = payload;
        if (entry->priority < wire::kPriorityNormal) {
            urgentQueue_.push_back(std::move(frame)); // 紧急通道不设溢出丢弃
        } else {
            if (static_cast<int>(normalQueue_.size()) >= settings_.sendQueueCapacity) {
                normalQueue_.erase(normalQueue_.begin()); // 溢出丢最旧（保新鲜）
                emit clientError(QString::fromLocal8Bit("TCP：普通发送队列溢出，丢弃最旧指令"));
            }
            normalQueue_.push_back(std::move(frame));
        }
    }
}

// ---------------------------------------------------------------- 工作线程

void TcpClient::initOnWorker()
{
    accumulator_ = std::make_unique<wire::FrameAccumulator>(settings_.maxPayload,
                                                            settings_.recvBufferLimit);

    socket_ = new QTcpSocket(); // 工作线程内创建（cleanup 中删除）
    socket_->setProxy(QNetworkProxy::NoProxy); // 板端直连，禁走系统代理
    if (settings_.noDelay) {
        socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    }
    connect(socket_, &QTcpSocket::connected, this, &TcpClient::onSocketConnected);
    connect(socket_, &QTcpSocket::disconnected, this, &TcpClient::onSocketDisconnected);
    connect(socket_, &QTcpSocket::readyRead, this, &TcpClient::onSocketReadyRead);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(socket_, &QTcpSocket::errorOccurred, this, [this] { onSocketError(); });
#endif

    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(kFlushTickMs);
    connect(flushTimer_, &QTimer::timeout, this, &TcpClient::onFlushTick);
    flushTimer_->start();

    const int sweepMs = std::max(25, settings_.requestTimeoutMs / 4);
    sweepTimer_ = new QTimer(this);
    sweepTimer_->setInterval(sweepMs);
    connect(sweepTimer_, &QTimer::timeout, this, &TcpClient::onSweepTick);
    sweepTimer_->start();

    if (settings_.heartbeatEnabled) {
        heartbeatTimer_ = new QTimer(this);
        heartbeatTimer_->setInterval(settings_.heartbeatIntervalMs);
        connect(heartbeatTimer_, &QTimer::timeout, this, &TcpClient::onHeartbeatTick);
        heartbeatTimer_->start();
    }

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, &TcpClient::tryReconnect);

    retryCount_ = 0;
    backoffMs_ = settings_.reconnectBaseMs;
    tryReconnect(); // 首次连接
}

void TcpClient::shutdownOnWorker()
{
    if (flushTimer_ != nullptr) {
        flushTimer_->stop();
        flushTimer_->deleteLater();
        flushTimer_ = nullptr;
    }
    if (sweepTimer_ != nullptr) {
        sweepTimer_->stop();
        sweepTimer_->deleteLater();
        sweepTimer_ = nullptr;
    }
    if (heartbeatTimer_ != nullptr) {
        heartbeatTimer_->stop();
        heartbeatTimer_->deleteLater();
        heartbeatTimer_ = nullptr;
    }
    if (reconnectTimer_ != nullptr) {
        reconnectTimer_->stop();
        reconnectTimer_->deleteLater();
        reconnectTimer_ = nullptr;
    }
    if (connectWatchdog_ != nullptr) {
        connectWatchdog_->stop();
        connectWatchdog_->deleteLater();
        connectWatchdog_ = nullptr;
    }
    if (socket_ != nullptr) {
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    {
        const std::lock_guard<std::mutex> lock(queueMutex_);
        clearQueuesLocked();
    }
    pendingAcks_.clear();
    accumulator_.reset();
    thread()->quit(); // 自终结事件循环
}

void TcpClient::onFlushTick()
{
    if ((socket_ == nullptr)
        || (socket_->state() != QAbstractSocket::ConnectedState)) {
        return;
    }
    // 紧急队列优先整体出队，其后普通队列（estop/emergency 永不排队等待）
    for (;;) {
        wire::WireFrame frame;
        {
            const std::lock_guard<std::mutex> lock(queueMutex_);
            if (!urgentQueue_.empty()) {
                frame = std::move(urgentQueue_.front());
                urgentQueue_.erase(urgentQueue_.begin());
            } else if (!normalQueue_.empty()) {
                frame = std::move(normalQueue_.front());
                normalQueue_.erase(normalQueue_.begin());
            } else {
                return;
            }
        }
        writeFrameNow(frame.funcId, frame.flags, frame.payload);
    }
}

void TcpClient::writeFrameNow(quint16 funcId, quint8 flags, const QByteArray& payload)
{
    const quint16 seq = nextSeq_;
    nextSeq_ = static_cast<quint16>((nextSeq_ + 1U) & 0xFFFFU); // 65535 回绕

    const QByteArray wire = wire::encodeFrame(funcId, seq, flags, payload,
                                              settings_.maxPayload);
    if (wire.isEmpty()) {
        emit clientError(QString::fromLocal8Bit("TCP：帧编码失败（funcId 0x%1）")
                             .arg(funcId, 4, 16, QLatin1Char('0')));
        return;
    }
    socket_->write(wire);

    const wire::FunctionEntry* entry = wire::FunctionRegistry::findByFuncId(funcId);
    if ((entry != nullptr) && entry->needsAck) {
        PendingAck pending;
        pending.funcId = funcId;
        pending.sentAtMs = QDateTime::currentMSecsSinceEpoch();
        pendingAcks_[seq] = pending;
    }
    emit requestSent(seq, funcId, payload);
}

void TcpClient::onSweepTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    std::vector<quint16> expired;
    for (const auto& item : pendingAcks_) {
        if ((now - item.second.sentAtMs) > settings_.requestTimeoutMs) {
            expired.push_back(item.first);
        }
    }
    for (const quint16 seq : expired) {
        const quint16 funcId = pendingAcks_[seq].funcId;
        pendingAcks_.erase(seq);
        emit requestTimedOut(seq, funcId); // 普通控制不盲目重发（状态未知红线）
    }
}

void TcpClient::onHeartbeatTick()
{
    if ((socket_ != nullptr)
        && (socket_->state() == QAbstractSocket::ConnectedState)) {
        writeFrameNow(static_cast<quint16>(wire::Func::Heartbeat), 0U,
                      wire::encodeHeartbeat(
                              static_cast<quint32>(QDateTime::currentMSecsSinceEpoch()
                                                   & 0xFFFFFFFFU)));
    }
}

void TcpClient::onSocketConnected()
{
    retryCount_ = 0;
    backoffMs_ = settings_.reconnectBaseMs;
    if (connectWatchdog_ != nullptr) {
        connectWatchdog_->stop();
    }
    connected_.store(true, std::memory_order_release);
    everConnected_.store(true, std::memory_order_release);
    if (announcedState_ != 1) {
        announcedState_ = 1;
        emit connectionStateChanged(true);
    }
    Logger::info(QString::fromLocal8Bit("TCP：已连接 %1:%2")
                     .arg(settings_.host)
                     .arg(settings_.port));

    // 重连后仅做只读权威状态查询（不重放旧舵机/推进器/模式命令红线）；
    // 三者均在注册表登记 needsAck，必须携带 ACK 请求标志
    writeFrameNow(static_cast<quint16>(wire::Func::Ask), wire::kFlagNeedAck, QByteArray());
    writeFrameNow(static_cast<quint16>(wire::Func::Status), wire::kFlagNeedAck, QByteArray());
    writeFrameNow(static_cast<quint16>(wire::Func::SensorAll), wire::kFlagNeedAck, QByteArray());
}

void TcpClient::onSocketDisconnected()
{
    const bool wasConnected = connected_.exchange(false, std::memory_order_acq_rel);
    if (wasConnected) {
        Logger::warning(QString::fromLocal8Bit("TCP：连接断开"));
    }
    if (announcedState_ != 0) {
        announcedState_ = 0; // 含首连尝试失败：让 UI 显示"离线/重连中"
        emit connectionStateChanged(false);
    }
    // 清空待发与挂起（断开即失效，防重放过期/危险指令）
    {
        const std::lock_guard<std::mutex> lock(queueMutex_);
        clearQueuesLocked();
    }
    pendingAcks_.clear();
    scheduleReconnectLocked();
}

void TcpClient::onSocketError()
{
    if (socket_ == nullptr) {
        return;
    }
    const QString text = socket_->errorString();
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        // 连接/重连阶段错误：不打断退避节奏，仅低频记录
        static qint64 lastLogMs = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if ((now - lastLogMs) > 2000) {
            lastLogMs = now;
            Logger::warning(QString::fromLocal8Bit("TCP：%1").arg(text));
        }
    }
}

void TcpClient::tryReconnect()
{
    if (!running_.load(std::memory_order_acquire) || (socket_ == nullptr)) {
        return;
    }
    if (socket_->state() == QAbstractSocket::ConnectedState) {
        return;
    }
    if ((settings_.maxRetry > 0) && (retryCount_ >= settings_.maxRetry)) {
        emit clientError(QString::fromLocal8Bit("TCP：达到最大重试次数 %1，停止重连")
                             .arg(settings_.maxRetry));
        return;
    }
    ++retryCount_;
    socket_->connectToHost(settings_.host, settings_.port);

    // 连接超时看门狗（connectTimeoutMs 内未建立即 abort 进入退避）
    if (connectWatchdog_ == nullptr) {
        connectWatchdog_ = new QTimer(this);
        connectWatchdog_->setSingleShot(true);
        connect(connectWatchdog_, &QTimer::timeout, this, [this] {
            if ((socket_ != nullptr)
                && (socket_->state() == QAbstractSocket::ConnectingState)) {
                socket_->abort();
                onSocketDisconnected();
            }
        });
    }
    connectWatchdog_->start(settings_.connectTimeoutMs);
}

void TcpClient::scheduleReconnectLocked()
{
    if (!settings_.reconnectEnabled || !running_.load(std::memory_order_acquire)) {
        return;
    }
    // 指数退避：base * 2^(retry-1)，封顶 max；幂次先钳位防整数溢出
    //（长时离线 retryCount 增长会使 2^n 超出 int 范围，曾致负间隔告警）
    const int shift = std::min(std::max(0, retryCount_ - 1), 20);
    const qint64 grown = static_cast<qint64>(settings_.reconnectBaseMs) << shift;
    const int delay = static_cast<int>(
            std::min(grown, static_cast<qint64>(settings_.reconnectMaxMs)));
    reconnectTimer_->start(delay);
    Logger::info(QString::fromLocal8Bit("TCP：%1ms 后重连（第 %2 次）")
                     .arg(delay)
                     .arg(retryCount_ + 1));
}

void TcpClient::onSocketReadyRead()
{
    if ((socket_ == nullptr) || (accumulator_ == nullptr)) {
        return;
    }
    const QByteArray chunk = socket_->readAll();
    accumulator_->feed(chunk);
    for (;;) {
        const wire::FrameAccumulator::NextResult r = accumulator_->next();
        if (r.error == wire::FrameError::Oversize) {
            emit clientError(QString::fromLocal8Bit("TCP：收到超长帧（len > %1），"
                                                    "判定失步并断线重连")
                                 .arg(settings_.maxPayload));
            socket_->abort(); // 缓存已清空，走断开/退避路径
            return;
        }
        if (r.error == wire::FrameError::Overflow) {
            emit clientError(QString::fromLocal8Bit("TCP：接收缓存溢出（> %1 字节），"
                                                    "断线重连")
                                 .arg(settings_.recvBufferLimit));
            socket_->abort();
            return;
        }
        if (!r.hasFrame) {
            return; // 等待更多数据
        }
        handleFrame(r.frame);
    }
}

void TcpClient::handleFrame(const wire::WireFrame& frame)
{
    switch (static_cast<wire::Func>(frame.funcId)) {
    case wire::Func::Ack: {
        wire::AckResult ack;
        if (!wire::decodeAck(frame.payload, ack)) {
            emit clientError(QString::fromLocal8Bit("TCP：ACK 载荷非法（丢弃）"));
            return;
        }
        const auto it = pendingAcks_.find(frame.seq);
        if (it == pendingAcks_.end()) {
            ++lateAckCount_; // 迟到/未知响应：计数丢弃，不更新任何控件
            emit lateAckDropped(frame.seq);
            return;
        }
        const quint16 funcId = it->second.funcId;
        pendingAcks_.erase(it);
        emit ackReceived(frame.seq, ack.errCode, funcId);
        return;
    }
    case wire::Func::SensorSummary: {
        wire::SensorSummary summary;
        if (!wire::decodeSensorSummary(frame.payload, summary)) {
            emit clientError(QString::fromLocal8Bit("TCP：传感器汇总帧非法（丢弃）"));
            return;
        }
        emit sensorSummaryReady(summary);
        return;
    }
    case wire::Func::Heartbeat:
        return; // 心跳响应：链路活性由发送侧超时判定
    case wire::Func::StateEvent:
    case wire::Func::AlarmEvent:
        emit eventReceived(frame.funcId, frame.payload);
        return;
    default: {
        const wire::FunctionEntry* entry =
                wire::FunctionRegistry::findByFuncId(frame.funcId);
        if (entry == nullptr) {
            ++unknownFuncCount_;
            emit clientError(QString::fromLocal8Bit("TCP：未知 funcId 0x%1（计数丢弃，"
                                                    "不断线）")
                                 .arg(frame.funcId, 4, 16, QLatin1Char('0')));
            return;
        }
        // 数据响应（get servo/propeller、ver/status/help 等）：交上层按注册表处理
        emit eventReceived(frame.funcId, frame.payload);
        return;
    }
    }
}

void TcpClient::clearQueuesLocked()
{
    urgentQueue_.clear();
    normalQueue_.clear();
}

} // namespace salacia
