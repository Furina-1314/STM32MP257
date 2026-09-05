#include "mock_a35.h"

#include <QDateTime>
#include <QTcpSocket>

using namespace salacia::wire;

MockA35::MockA35(QObject* parent)
    : QObject(parent)
    , acc_(4096, 65536)
{
    connect(&server_, &QTcpServer::newConnection, this, &MockA35::onNewConnection);
    connect(&streamTimer_, &QTimer::timeout, this, &MockA35::sendSummaryOnce);
    streamTimer_.setTimerType(Qt::PreciseTimer);
}

bool MockA35::start()
{
    return server_.listen(QHostAddress::LocalHost, 0);
}

void MockA35::onNewConnection()
{
    if (client_ != nullptr) {
        client_->abort();
        client_->deleteLater();
    }
    client_ = server_.nextPendingConnection();
    frameCount_ = 0;
    acc_.reset();
    connect(client_, &QTcpSocket::readyRead, this, &MockA35::onReadyRead);
    connect(client_, &QTcpSocket::disconnected, this, &MockA35::clientDisconnected);
    emit clientConnected();
}

void MockA35::onReadyRead()
{
    if (client_ == nullptr) {
        return;
    }
    acc_.feed(client_->readAll());
    for (;;) {
        const FrameAccumulator::NextResult r = acc_.next();
        if (!r.hasFrame) {
            return;
        }
        ++frameCount_;
        received_.append(qMakePair(r.frame.funcId, r.frame.payload));
        arrivals_.append(QDateTime::currentMSecsSinceEpoch());
        emit gotFrame(r.frame.funcId, r.frame.seq, r.frame.payload);

        if (autoAck && (r.frame.flags & kFlagNeedAck) != 0U) {
            const quint16 seq = r.frame.seq;
            const quint16 err = errorMap_.value(r.frame.funcId, ackErrorCode);
            const int delay = ackDelayMs;
            if (delay > 0) {
                QTimer::singleShot(delay, this, [this, seq, err] {
                    sendAckWith(seq, err);
                });
            } else {
                sendAckWith(seq, err);
            }
        }
        if ((dropAfterFrames > 0) && (frameCount_ >= dropAfterFrames)) {
            QTimer::singleShot(0, this, &MockA35::dropClient);
        }
    }
}

void MockA35::sendFrame(quint16 funcId, quint16 seq, quint8 flags,
                        const QByteArray& payload)
{
    if (client_ == nullptr) {
        return;
    }
    const QByteArray wire = encodeFrame(funcId, seq, flags, payload, 4096);
    if (ackSplits <= 1) {
        client_->write(wire);
        return;
    }
    const int step = qMax(1, static_cast<int>(wire.size()) / ackSplits);
    for (int off = 0; off < wire.size(); off += step) {
        client_->write(wire.mid(off, step));
    }
}

void MockA35::sendLateAck(quint16 seq)
{
    sendAckWith(seq, ackErrorCode);
}

void MockA35::sendAckWith(quint16 seq, quint16 errCode)
{
    QByteArray payload;
    putU16(payload, errCode);
    sendFrame(static_cast<quint16>(Func::Ack), seq, 0U, payload);
}

void MockA35::sendSummaryOnce()
{
    if (client_ == nullptr) {
        return;
    }
    for (int i = 0; i < framesPerTick_; ++i) {
        sendOneSummaryFrame();
    }
}

void MockA35::sendOneSummaryFrame()
{
    QByteArray payload;
    putF32(payload, 26.5F);
    putF32(payload, 56.7F);
    putF32(payload, 0.1F); putF32(payload, -0.2F); putF32(payload, 9.8F);
    putF32(payload, 0.01F); putF32(payload, 0.02F); putF32(payload, -0.03F);
    putF32(payload, 15.2F);
    putF32(payload, 350.0F);
    payload.append(static_cast<char>(kValidTempHum | kValidMpu | kValidVoltage | kValidDyp));
    putU32(payload, 12345U);
    sendFrame(static_cast<quint16>(Func::SensorSummary), 0U, kFlagEvent, payload);
}

void MockA35::sendRaw(const QByteArray& bytes)
{
    if (client_ != nullptr) {
        client_->write(bytes);
    }
}

void MockA35::dropClient()
{
    if (client_ != nullptr) {
        client_->abort();
    }
}

void MockA35::sendStateEvent(quint8 mask)
{
    QByteArray payload;
    payload.append(static_cast<char>(mask));
    sendFrame(static_cast<quint16>(Func::StateEvent), 0U, kFlagEvent, payload);
}

void MockA35::sendStateEventV2(quint16 mask)
{
    QByteArray payload;
    payload.append(static_cast<char>(kStateEventV2Version));
    putU16(payload, mask);
    sendFrame(static_cast<quint16>(Func::StateEventV2), 0U, kFlagEvent, payload);
}

void MockA35::setAckErrorCodeFor(quint16 funcId, quint16 errCode)
{
    errorMap_.insert(funcId, errCode);
}

void MockA35::startStream(int hz)
{
    // Windows 定时器合并（~15.6ms）使 >64Hz 单帧定时不可达：
    // 以 >=20ms 周期每 tick 突发多帧实现目标均值速率
    framesPerTick_ = qMax(1, hz / 50);
    const int interval = qMax(20, 1000 * framesPerTick_ / qMax(1, hz));
    streamTimer_.start(interval);
}

void MockA35::stopStream()
{
    streamTimer_.stop();
}

