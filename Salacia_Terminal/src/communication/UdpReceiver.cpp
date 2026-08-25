#include "UdpReceiver.h"

#include <QDateTime>
#include <QThread>
#include <QUdpSocket>
#include <QTimer>

#include "communication/TelemetryPacket.h"
#include "core/AppConfig.h"
#include "core/DataManager.h"
#include "core/Logger.h"

namespace salacia {

namespace {
// 看门狗：500ms 周期检查，1 秒无有效包判离线（20Hz 下丢 20 包）
constexpr int kWatchdogIntervalMs = 500;
constexpr int kTelemetryTimeoutMs = 1000;
} // namespace

UdpReceiver::UdpReceiver(QObject* parent)
    : QObject(parent)
{
}

UdpReceiver::~UdpReceiver()
{
    stop(); // RAII 兜底（幂等）
}

void UdpReceiver::start()
{
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const AppConfig& cfg = AppConfig::instance();
    processor_.setGains(cfg.mahonyKp(), cfg.mahonyKi());
    processor_.reset();

    worker_ = std::make_unique<QThread>();
    worker_->setObjectName(QStringLiteral("salacia-telemetry"));
    moveToThread(worker_.get());
    connect(worker_.get(), &QThread::started, this, &UdpReceiver::initOnWorker);
    worker_->start();
    Logger::info(QString::fromLocal8Bit("遥测：接收线程已启动"));
}

void UdpReceiver::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // 在工作线程内安全关闭 socket/定时器（阻塞等待），再退出线程
    QMetaObject::invokeMethod(this, &UdpReceiver::cleanupOnWorker,
                              Qt::BlockingQueuedConnection);
    if (worker_ != nullptr) {
        worker_->quit();
        worker_->wait();
    }
    DataManager::instance().setTelemetryActive(false);
    Logger::info(QString::fromLocal8Bit("遥测：接收线程已停止"));
}

void UdpReceiver::initOnWorker()
{
    const quint16 port = AppConfig::instance().telemetryPort();

    socket_ = new QUdpSocket(this); // 父对象已在工作线程
    if (!socket_->bind(QHostAddress::AnyIPv4, port,
                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit receiverError(QString::fromLocal8Bit("遥测端口 %1 绑定失败：%2")
                               .arg(port)
                               .arg(socket_->errorString()));
        Logger::error(QString::fromLocal8Bit("遥测：端口 %1 绑定失败").arg(port));
        return;
    }
    connect(socket_, &QUdpSocket::readyRead, this, &UdpReceiver::readPending);

    watchdog_ = new QTimer(this);
    watchdog_->setInterval(kWatchdogIntervalMs);
    connect(watchdog_, &QTimer::timeout, this, &UdpReceiver::checkWatchdog);
    watchdog_->start();

    lastPacketMs_.store(0, std::memory_order_release);
    Logger::info(QString::fromLocal8Bit("遥测：监听 0.0.0.0:%1（Mahony Kp=%2 Ki=%3）")
                     .arg(port)
                     .arg(AppConfig::instance().mahonyKp())
                     .arg(AppConfig::instance().mahonyKi()));
}

void UdpReceiver::cleanupOnWorker()
{
    if (watchdog_ != nullptr) {
        watchdog_->stop();
        watchdog_->deleteLater();
        watchdog_ = nullptr;
    }
    if (socket_ != nullptr) {
        socket_->close();
        socket_->deleteLater();
        socket_ = nullptr;
    }
}

void UdpReceiver::readPending()
{
    if (socket_ == nullptr) {
        return;
    }

    while (socket_->hasPendingDatagrams()) {
        const qint64 size = socket_->pendingDatagramSize();
        if ((size <= 0) || (size > 512)) { // 边界防御：异常大包直接丢弃
            char sink[512];
            const qint64 n = socket_->readDatagram(sink, sizeof(sink));
            if (n > 0) {
                dropped_.fetch_add(1, std::memory_order_acq_rel);
            }
            continue;
        }

        QByteArray datagram(static_cast<int>(size), Qt::Uninitialized);
        const qint64 read = socket_->readDatagram(datagram.data(), size);
        if (read != size) {
            continue;
        }

        RawImuSample sample;
        if (!parseTelemetryPacket(datagram.constData(), read, sample)) {
            dropped_.fetch_add(1, std::memory_order_acq_rel);
            continue; // 校验失败：静默丢弃并计数
        }

        sample.hostTimeMs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
        RovState state = processor_.process(sample);

        // 电池电量折算（满/空电压来自 app_config.ini，参数解耦红线）
        const float full = static_cast<float>(AppConfig::instance().batteryFullVoltage());
        const float empty = static_cast<float>(AppConfig::instance().batteryEmptyVoltage());
        if (full > empty) {
            const float pct = (sample.batteryVoltage - empty) / (full - empty) * 100.0F;
            state.batteryPercent = (pct < 0.0F) ? 0.0F : ((pct > 100.0F) ? 100.0F : pct);
        }
        DataManager::instance().setRovState(state);

        received_.fetch_add(1, std::memory_order_acq_rel);
        lastPacketMs_.store(static_cast<qint64>(sample.hostTimeMs),
                            std::memory_order_release);
    }
}

void UdpReceiver::checkWatchdog()
{
    const qint64 last = lastPacketMs_.load(std::memory_order_acquire);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if ((last > 0) && ((now - last) > kTelemetryTimeoutMs)) {
        if (active_.exchange(false, std::memory_order_acq_rel)) {
            DataManager::instance().setTelemetryActive(false);
            emit telemetryActiveChanged(false);
        }
    } else if (last > 0) {
        if (!active_.exchange(true, std::memory_order_acq_rel)) {
            DataManager::instance().setTelemetryActive(true);
            emit telemetryActiveChanged(true);
        }
    }
}

} // namespace salacia
