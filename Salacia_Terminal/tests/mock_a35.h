#pragma once

#include <QMap>
#include <QObject>
#include <QPair>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

#include "communication/FunctionRegistry.h"
#include "communication/WireCodec.h"

// mock A35 服务端（仅测试用；行为可配置）
//
// 能力：自动 ACK（可配错误码/延迟/分段写回/按函数定制错误码）、
// 100Hz 传感器汇总流（突发式绕 Windows 定时器合并）、计数断连、
// 状态事件注入（权限矩阵驱动）、原始字节注入（坏帧/超长帧）、
// 帧到达时刻记录（性能测量）。
class MockA35 : public QObject
{
    Q_OBJECT

public:
    explicit MockA35(QObject* parent = nullptr);

    bool start();
    quint16 port() const { return server_.serverPort(); }

    // ---- 测试行为开关 ----
    bool autoAck = true;
    quint16 ackErrorCode = 0U;
    int ackDelayMs = 0;
    int ackSplits = 1;
    int dropAfterFrames = 0;

    const QVector<QPair<quint16, QByteArray>>& received() const { return received_; }
    const QVector<qint64>& arrivalTimesMs() const { return arrivals_; }

signals:
    void gotFrame(quint16 funcId, quint16 seq, const QByteArray& payload);
    void clientConnected();
    void clientDisconnected();

public slots:
    void sendSummaryOnce();
    void sendRaw(const QByteArray&);
    void sendLateAck(quint16 seq);
    void dropClient();
    void sendStateEvent(quint8 mask);      // legacy 0x0102
    void sendStateEventV2(quint16 mask);   // 0x0104（主链路）
    void setAckErrorCodeFor(quint16 funcId, quint16 errCode);
    void startStream(int hz);
    void stopStream();

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    void sendAckWith(quint16 seq, quint16 errCode);
    void sendOneSummaryFrame();
    void sendFrame(quint16 funcId, quint16 seq, quint8 flags, const QByteArray& payload);

    QTcpServer server_;
    QTcpSocket* client_ = nullptr;
    salacia::wire::FrameAccumulator acc_;
    QTimer streamTimer_;
    QVector<QPair<quint16, QByteArray>> received_;
    QVector<qint64> arrivals_;
    QMap<quint16, quint16> errorMap_;
    int frameCount_ = 0;
    int framesPerTick_ = 1;
};
