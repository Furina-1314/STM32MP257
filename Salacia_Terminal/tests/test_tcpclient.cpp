// TcpClient + mock A35 集成测试（真实 socket 回环，工作线程客户端）
#include <QtTest/qtest.h>  // 窄化包含：避开 QtCore 伞头（qrunnable.h 预览包缺陷）

#include <QSignalSpy>

#include "mock_a35.h"
#include "communication/TcpClient.h"
#include "communication/FunctionRegistry.h"
#include "communication/WireCodec.h"

using namespace salacia;
using namespace salacia::wire;

namespace {

TcpClient::Settings fastSettings(quint16 port)
{
    TcpClient::Settings s;
    s.host = QStringLiteral("127.0.0.1");
    s.port = port;
    s.connectTimeoutMs = 1500;
    s.requestTimeoutMs = 300;
    s.heartbeatEnabled = false;
    s.heartbeatIntervalMs = 1000;
    s.reconnectEnabled = true;
    s.reconnectBaseMs = 200;  // 测试用快速退避
    s.reconnectMaxMs = 400;
    s.maxRetry = 0;
    s.noDelay = true;
    s.recvBufferLimit = 65536;
    s.maxPayload = 4096;
    s.sendQueueCapacity = 64;
    return s;
}

bool waitForSpy(QSignalSpy& spy, int timeoutMs)
{
    return spy.wait(timeoutMs) || (spy.count() > 0);
}

} // namespace

class TestTcpClient : public QObject
{
    Q_OBJECT

private slots:
    void connectAndAuthorityQueries();
    void servoSetRoundtrip();
    void estopSingleFrame();
    void priorityWhileConnecting();
    void urgentOrderWithinQueue();
    void ackMatching();
    void timeoutAndLateAck();
    void sensorStreamHighRate();
    void oversizeCausesDisconnect();
    void reconnectAfterDrop();
    void noReplayAfterDrop();
    void heartbeatSeen();
    void chunkedAckResync();

};

void TestTcpClient::connectAndAuthorityQueries()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(fastSettings(mock.port()));
    QVERIFY(waitForSpy(stateSpy, 3000));
    QCOMPARE(stateSpy.first().at(0).toBool(), true);
    // 重连后仅发送只读权威查询：ask/status/sensor all
    QSignalSpy frameSpy(&mock, &MockA35::gotFrame);
    QVERIFY(waitForSpy(frameSpy, 2000));
    QTest::qWait(200);
    QVector<quint16> funcs;
    for (const auto& args : frameSpy) {
        funcs.append(args.at(0).toUInt());
    }
    QVERIFY(funcs.contains(static_cast<quint16>(Func::Ask)));
    QVERIFY(funcs.contains(static_cast<quint16>(Func::Status)));
    QVERIFY(funcs.contains(static_cast<quint16>(Func::SensorAll)));
    client.stop();
}

void TestTcpClient::servoSetRoundtrip()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(fastSettings(mock.port()));
    QVERIFY(waitForSpy(stateSpy, 3000));
    QTest::qWait(100); // 权威查询先走

    ServoSetCmd cmd;
    cmd.id = 2U;
    cmd.angleDeg = 135U;
    QSignalSpy frameSpy(&mock, &MockA35::gotFrame);
    client.sendFrame(static_cast<quint16>(Func::ServoSet), encodeServoSet(cmd));
    QVERIFY(waitForSpy(frameSpy, 2000));
    // 找到 ServoSet 帧
    bool found = false;
    for (const auto& args : frameSpy) {
        if (args.at(0).toUInt() == static_cast<quint16>(Func::ServoSet)) {
            const QByteArray payload = args.at(2).toByteArray();
            QCOMPARE(payload.size(), 3);
            QCOMPARE(static_cast<quint8>(payload.at(0)), 2U);
            bool ok = false;
            QCOMPARE(getU16(payload, 1, ok), 135U);
            QVERIFY(ok);
            found = true;
        }
    }
    QVERIFY(found);
    client.stop();
}

void TestTcpClient::estopSingleFrame()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(fastSettings(mock.port()));
    QVERIFY(waitForSpy(stateSpy, 3000));
    QTest::qWait(100);

    QSignalSpy frameSpy(&mock, &MockA35::gotFrame);
    const int before = mock.received().size();
    // Estop 空载荷：仅推进器置零语义，绝不携带舵机角度
    client.sendFrame(static_cast<quint16>(Func::Estop), QByteArray());
    QVERIFY(waitForSpy(frameSpy, 2000));
    QTest::qWait(150);
    int estopCount = 0;
    for (int i = before; i < mock.received().size(); ++i) {
        if (mock.received().at(i).first == static_cast<quint16>(Func::Estop)) {
            ++estopCount;
            QCOMPARE(mock.received().at(i).second.size(), 0); // 载荷为空（无舵机字节）
        }
    }
    QCOMPARE(estopCount, 1); // 严格单帧
    client.stop();
}

void TestTcpClient::priorityWhileConnecting()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient client;
    // 客户端启动后立即入队：普通控制 5 帧 + estop——建立连接后 estop 必须最先到达
    PropellerSetCmd cmd;
    cmd.id = 12U; // wire 12 = 垂直3（UI）
    cmd.valuePct = 40;
    const QByteArray normal = encodePropellerSet(cmd);
    const QByteArray urgent; // estop 空载荷
    const int before = mock.received().size();
    client.start(fastSettings(mock.port()));
    for (int i = 0; i < 5; ++i) {
        client.sendFrame(static_cast<quint16>(Func::PropellerSet), normal);
    }
    client.sendFrame(static_cast<quint16>(Func::Estop), urgent);

    QSignalSpy frameSpy(&mock, &MockA35::gotFrame);
    QVERIFY(waitForSpy(frameSpy, 3000));
    QTest::qWait(300);
    QVERIFY(mock.received().size() - before >= 6);
    // 权威查询（3 帧）之后紧跟的第一个控制帧必须是 estop
    const auto& all = mock.received();
    int firstControl = -1;
    for (int i = before; i < all.size(); ++i) {
        const quint16 f = all.at(i).first;
        const bool authority = (f == static_cast<quint16>(Func::Ask))
                || (f == static_cast<quint16>(Func::Status))
                || (f == static_cast<quint16>(Func::SensorAll));
        if (!authority) {
            firstControl = i;
            break;
        }
    }
    QVERIFY(firstControl >= 0);
    QCOMPARE(all.at(firstControl).first, static_cast<quint16>(Func::Estop));
    client.stop();
}

void TestTcpClient::urgentOrderWithinQueue()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient client;

    // 入队顺序刻意逆序（普通 -> StopAll -> Emergency -> Estop）：
    // 紧急队列按优先级稳定排序，到达次序必须为 Estop > Emergency > StopAll
    PropellerSetCmd cmd;
    cmd.id = 11U; // wire 11 = 垂直2（UI）
    cmd.valuePct = 30;
    const QByteArray normal = encodePropellerSet(cmd);
    const int before = mock.received().size();
    client.start(fastSettings(mock.port()));
    for (int i = 0; i < 4; ++i) {
        client.sendFrame(static_cast<quint16>(Func::PropellerSet), normal);
    }
    client.sendFrame(static_cast<quint16>(Func::StopAll), QByteArray());
    client.sendFrame(static_cast<quint16>(Func::Emergency), QByteArray());
    client.sendFrame(static_cast<quint16>(Func::Estop), QByteArray());

    // 等待全部到达（4 普通 + 3 紧急 + 3 权威查询）
    const int expectedTotal = 4 + 3 + 3;
    for (int guard = 0; guard < 400; ++guard) {
        if (mock.received().size() - before >= expectedTotal) {
            break;
        }
        QTest::qWait(10);
    }
    const auto& all = mock.received();
    int estopIdx = -1;
    int emgIdx = -1;
    int stopIdx = -1;
    for (int i = before; i < all.size(); ++i) {
        const quint16 f = all.at(i).first;
        if (f == static_cast<quint16>(Func::Estop)) {
            estopIdx = i;
        } else if (f == static_cast<quint16>(Func::Emergency)) {
            emgIdx = i;
        } else if (f == static_cast<quint16>(Func::StopAll)) {
            stopIdx = i;
        }
    }
    QVERIFY(estopIdx >= 0);
    QVERIFY(emgIdx >= 0);
    QVERIFY(stopIdx >= 0);
    // 插队次序红线：Estop > Emergency > Stop/Move
    QVERIFY2(estopIdx < emgIdx, "estop must precede emergency");
    QVERIFY2(emgIdx < stopIdx, "emergency must precede stop all");
    client.stop();
}

void TestTcpClient::ackMatching()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(fastSettings(mock.port()));
    QVERIFY(waitForSpy(stateSpy, 3000));
    QTest::qWait(100);

    ServoSetCmd cmd;
    cmd.id = 1U;
    cmd.angleDeg = 90U;
    QSignalSpy sentSpy(&client, &TcpClient::requestSent);
    QSignalSpy ackSpy(&client, &TcpClient::ackReceived);
    QSignalSpy timeoutSpy(&client, &TcpClient::requestTimedOut);
    client.sendFrame(static_cast<quint16>(Func::ServoSet), encodeServoSet(cmd));
    QVERIFY(waitForSpy(ackSpy, 2000));
    QCOMPARE(ackSpy.first().at(1).toUInt(), 0U); // errCode = ok
    QCOMPARE(ackSpy.first().at(2).toUInt(),
             static_cast<uint>(static_cast<quint16>(Func::ServoSet)));
    QTest::qWait(500);
    QCOMPARE(timeoutSpy.count(), 0); // 未误报超时
    client.stop();
}

void TestTcpClient::timeoutAndLateAck()
{
    MockA35 mock;
    QVERIFY(mock.start());
    mock.autoAck = false;
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(fastSettings(mock.port()));
    QVERIFY(waitForSpy(stateSpy, 3000));
    QTest::qWait(100);

    QSignalSpy timeoutSpy(&client, &TcpClient::requestTimedOut);
    QSignalSpy ackSpy(&client, &TcpClient::ackReceived);
    QSignalSpy lateSpy(&client, &TcpClient::lateAckDropped);
    ServoSetCmd cmd;
    cmd.id = 1U;
    cmd.angleDeg = 45U;
    QSignalSpy sentSpy(&client, &TcpClient::requestSent);
    client.sendFrame(static_cast<quint16>(Func::ServoSet), encodeServoSet(cmd));
    QVERIFY(waitForSpy(sentSpy, 2000));
    const quint16 seq = static_cast<quint16>(sentSpy.first().at(0).toUInt());

    QVERIFY(waitForSpy(timeoutSpy, 1500)); // request_timeout=300ms（首报为权威查询）
    QTest::qWait(600); // 等 ServoSet 自身的超时窗走完
    // 本用例 mock 不回 ACK：权威查询(ask 等)也会超时——在全部超时里定位 ServoSet
    bool servoTimedOut = false;
    for (const auto& args : timeoutSpy) {
        if (args.at(1).toUInt() == static_cast<uint>(static_cast<quint16>(Func::ServoSet))) {
            servoTimedOut = true;
        }
    }
    QVERIFY(servoTimedOut);
    const int timeoutsBeforeLateAck = timeoutSpy.count();

    // 迟到 ACK：不更新任何请求状态，仅计数丢弃
    QMetaObject::invokeMethod(&mock, "sendLateAck", Qt::QueuedConnection,
                              Q_ARG(quint16, seq));
    QVERIFY(waitForSpy(lateSpy, 2000));
    QCOMPARE(lateSpy.first().at(0).toUInt(), static_cast<uint>(seq));
    QCOMPARE(ackSpy.count(), 0);
    QCOMPARE(timeoutSpy.count(), timeoutsBeforeLateAck); // 迟到响应不产生新超时
    client.stop();
}

void TestTcpClient::sensorStreamHighRate()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(fastSettings(mock.port()));
    QVERIFY(waitForSpy(stateSpy, 3000));

    QSignalSpy summarySpy(&client, &TcpClient::sensorSummaryReady);
    QMetaObject::invokeMethod(&mock, "startStream", Qt::QueuedConnection,
                              Q_ARG(int, 100));
    QTest::qWait(1000);
    QMetaObject::invokeMethod(&mock, "stopStream", Qt::QueuedConnection);
    QTest::qWait(300);
    // 100Hz x 1s：允许调度抖动，不得低于 80%
    QVERIFY2(summarySpy.count() >= 80,
             qPrintable(QStringLiteral("summary count = %1").arg(summarySpy.count())));
    // 事件循环未被阻塞（qWait 本身即证明）；首帧数值抽查
    if (summarySpy.count() > 0) {
        const SensorSummary s = summarySpy.first().at(0)
                .value<SensorSummary>();
        QCOMPARE(s.tempC, 26.5F);
        QCOMPARE(s.voltage, 15.2F);
    }
    client.stop();
}

void TestTcpClient::oversizeCausesDisconnect()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(fastSettings(mock.port()));
    QVERIFY(waitForSpy(stateSpy, 3000));
    QTest::qWait(100);
    QCOMPARE(stateSpy.count(), 1);

    // 手工构造 len=8192 的超长帧头
    QByteArray evil;
    putU32(evil, kMagic);
    evil.append(static_cast<char>(kVersion));
    putU16(evil, static_cast<quint16>(Func::Ask));
    putU16(evil, 1U);
    evil.append(char(0));
    putU16(evil, 8192U);
    evil.append(QByteArray(64, char(0)));
    QMetaObject::invokeMethod(&mock, "sendRaw", Qt::QueuedConnection,
                              Q_ARG(QByteArray, evil));
    QVERIFY(waitForSpy(stateSpy, 3000)); // 断开事件
    QCOMPARE(stateSpy.last().at(0).toBool(), false);
    client.stop();
}

void TestTcpClient::reconnectAfterDrop()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    QSignalSpy serverConnSpy(&mock, &MockA35::clientConnected);
    client.start(fastSettings(mock.port()));
    QVERIFY(waitForSpy(stateSpy, 3000));
    QCOMPARE(serverConnSpy.count(), 1);

    QSignalSpy dropSpy(&mock, &MockA35::clientDisconnected);
    QMetaObject::invokeMethod(&mock, "dropClient", Qt::QueuedConnection);
    QVERIFY(waitForSpy(stateSpy, 2000)); // 掉线
    QCOMPARE(stateSpy.last().at(0).toBool(), false);

    QVERIFY(waitForSpy(stateSpy, 3000)); // 退避重连成功
    QCOMPARE(stateSpy.last().at(0).toBool(), true);
    QCOMPARE(serverConnSpy.count(), 2);
    client.stop();
}

void TestTcpClient::noReplayAfterDrop()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient::Settings s = fastSettings(mock.port());
    s.reconnectBaseMs = 800; // 拉长退避，留出"断开窗口"稳定发送被拒指令
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(s);
    QVERIFY(waitForSpy(stateSpy, 3000));
    QTest::qWait(100);

    QMetaObject::invokeMethod(&mock, "dropClient", Qt::QueuedConnection);
    // 等待断开事件（而非固定 sleep，规避退避竞态）
    int guard = 0;
    while (client.isConnected() && (guard < 50)) {
        QTest::qWait(20);
        ++guard;
    }
    QVERIFY(!client.isConnected());

    // 掉线期间下发普通控制：应被拒绝（不入队）
    QSignalSpy errSpy(&client, &TcpClient::clientError);
    ServoSetCmd cmd;
    cmd.id = 4U;
    cmd.angleDeg = 30U;
    const int before = mock.received().size();
    for (int i = 0; i < 3; ++i) {
        client.sendFrame(static_cast<quint16>(Func::ServoSet), encodeServoSet(cmd));
    }
    QVERIFY(waitForSpy(errSpy, 1000));
    QCOMPARE(errSpy.count(), 3);

    // 重连后 mock 只应见到权威查询，绝不出现 ServoSet
    QVERIFY(waitForSpy(stateSpy, 4000));
    QCOMPARE(stateSpy.last().at(0).toBool(), true);
    QTest::qWait(300);
    for (int i = before; i < mock.received().size(); ++i) {
        QVERIFY2(mock.received().at(i).first != static_cast<quint16>(Func::ServoSet),
                 "断线期间的指令不得在重连后重放");
    }
    client.stop();
}

void TestTcpClient::heartbeatSeen()
{
    MockA35 mock;
    QVERIFY(mock.start());
    TcpClient::Settings s = fastSettings(mock.port());
    s.heartbeatEnabled = true;
    s.heartbeatIntervalMs = 100;
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(s);
    QVERIFY(waitForSpy(stateSpy, 3000));

    QSignalSpy frameSpy(&mock, &MockA35::gotFrame);
    QTest::qWait(500);
    bool sawHeartbeat = false;
    for (const auto& args : frameSpy) {
        if (args.at(0).toUInt() == static_cast<quint16>(Func::Heartbeat)) {
            sawHeartbeat = true;
        }
    }
    QVERIFY(sawHeartbeat);
    client.stop();
}

void TestTcpClient::chunkedAckResync()
{
    MockA35 mock;
    QVERIFY(mock.start());
    mock.ackSplits = 5; // ACK 拆 5 段写回（半包到达）
    TcpClient client;
    QSignalSpy stateSpy(&client, &TcpClient::connectionStateChanged);
    client.start(fastSettings(mock.port()));
    QVERIFY(waitForSpy(stateSpy, 3000));
    QTest::qWait(100);

    QSignalSpy ackSpy(&client, &TcpClient::ackReceived);
    ServoSetCmd cmd;
    cmd.id = 7U;
    cmd.angleDeg = 10U;
    client.sendFrame(static_cast<quint16>(Func::ServoSet), encodeServoSet(cmd));
    QVERIFY(waitForSpy(ackSpy, 2000)); // 分段到达仍能完整组帧并匹配
    client.stop();
}

QTEST_MAIN(TestTcpClient)
#include "test_tcpclient.moc"
