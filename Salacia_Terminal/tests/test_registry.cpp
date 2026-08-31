// FunctionRegistry 单元测试：表完整性、estop 单帧零值、值域校验、解码
#include <QtTest/qtest.h>  // 窄化包含：避开 QtCore 伞头（qthreadpool->qrunnable 的 const 默认构造与 /permissive- 冲突）

#include <cmath>
#include <limits>

#include "communication/FunctionRegistry.h"
#include "communication/WireCodec.h"

using namespace salacia::wire;

class TestRegistry : public QObject
{
    Q_OBJECT

private slots:
    void tableComplete()
    {
        QVERIFY(FunctionRegistry::count() >= 26);
        const auto& table = FunctionRegistry::all();

        // funcId 唯一
        for (std::size_t i = 0; i < table.size(); ++i) {
            for (std::size_t j = i + 1; j < table.size(); ++j) {
                QVERIFY2(table[i].funcId != table[j].funcId, "funcId must be unique");
            }
        }
        // 名称唯一
        for (std::size_t i = 0; i < table.size(); ++i) {
            for (std::size_t j = i + 1; j < table.size(); ++j) {
                QVERIFY2(QString::fromLatin1(table[i].name)
                                 != QString::fromLatin1(table[j].name),
                         "name must be unique");
            }
        }
        // estop 与 stop/emergency 的 funcId/优先级严格分离
        const FunctionEntry* estop = FunctionRegistry::findByName(
                QStringLiteral("estop"));
        const FunctionEntry* stop = FunctionRegistry::findByName(QStringLiteral("stop"));
        const FunctionEntry* emg = FunctionRegistry::findByName(QStringLiteral("emergency"));
        QVERIFY(estop != nullptr);
        QVERIFY(stop != nullptr);
        QVERIFY(emg != nullptr);
        QVERIFY(estop->funcId != stop->funcId);
        QVERIFY(estop->funcId != emg->funcId);
        QVERIFY(estop->priority < stop->priority);
        QCOMPARE(estop->priority, kPriorityEstop);
    }

    void lookupUnknown()
    {
        QVERIFY(FunctionRegistry::findByFuncId(0xABCDU) == nullptr);
        QVERIFY(FunctionRegistry::findByName(QStringLiteral("shell")) == nullptr);
        const FunctionEntry* servo = FunctionRegistry::findByFuncId(
                static_cast<quint16>(Func::ServoSet));
        QVERIFY(servo != nullptr);
        QCOMPARE(QString::fromLatin1(servo->name), QStringLiteral("set servo"));
        QVERIFY(servo->needsAck);
    }

    void estopSingleFrame()
    {
        const QByteArray payload = encodeEstop(10, 6);
        QCOMPARE(payload.size(), 32); // 10*u16 + 6*i16，全部零值单帧
        for (int i = 0; i < payload.size(); ++i) {
            QCOMPARE(payload.at(i), char(0));
        }
        // 非法数量拒绝
        QVERIFY(encodeEstop(0, 6).isEmpty());
        QVERIFY(encodeEstop(10, 0).isEmpty());
        QVERIFY(encodeEstop(17, 6).isEmpty());
    }

    void servoSetValueRange()
    {
        ServoSetCmd cmd;
        cmd.id = 2U;
        cmd.angleDeg = 135U;
        const QByteArray ok = encodeServoSet(cmd);
        QCOMPARE(ok.size(), 3);
        bool flag = true;
        QCOMPARE(static_cast<quint8>(ok.at(0)), 2U);
        QCOMPARE(getU16(ok, 1, flag), 135U);
        QVERIFY(flag);

        cmd.angleDeg = 181U;
        QVERIFY(encodeServoSet(cmd).isEmpty()); // 值域红线
        cmd.angleDeg = 0U;
        QVERIFY(!encodeServoSet(cmd).isEmpty());
    }

    void propellerSetValueRange()
    {
        PropellerSetCmd cmd;
        cmd.id = 5U;
        cmd.valuePct = -80;
        const QByteArray ok = encodePropellerSet(cmd);
        QCOMPARE(ok.size(), 3);
        bool flag = true;
        QCOMPARE(getI16(ok, 1, flag), -80);
        QVERIFY(flag);

        cmd.valuePct = 101;
        QVERIFY(encodePropellerSet(cmd).isEmpty());
        cmd.valuePct = -101;
        QVERIFY(encodePropellerSet(cmd).isEmpty());
        cmd.valuePct = 100;
        QVERIFY(!encodePropellerSet(cmd).isEmpty());
    }

    void baseValueFrame()
    {
        const QByteArray payload = encodeBaseValue(6, -35);
        QCOMPARE(payload.size(), 12);
        bool flag = true;
        for (int i = 0; i < 6; ++i) {
            QCOMPARE(getI16(payload, i * 2, flag), -35);
        }
        QVERIFY(flag);
        QVERIFY(encodeBaseValue(6, 101).isEmpty());
    }

    void heartbeatPayload()
    {
        const QByteArray p = encodeHeartbeat(123456789U);
        QCOMPARE(p.size(), 4);
        bool flag = true;
        QCOMPARE(getU32(p, 0, flag), 123456789U);
        QVERIFY(flag);
    }

    void sensorSummaryDecode()
    {
        QByteArray p;
        putF32(p, 26.5F);
        putF32(p, 56.7F);
        putF32(p, 0.1F); putF32(p, -0.2F); putF32(p, 9.8F);
        putF32(p, 0.01F); putF32(p, 0.02F); putF32(p, -0.03F);
        putF32(p, 15.2F);
        putF32(p, 350.0F);
        p.append(static_cast<char>(kValidTempHum | kValidMpu | kValidVoltage | kValidDyp));
        putU32(p, 98765U);

        SensorSummary s;
        QVERIFY(decodeSensorSummary(p, s));
        QCOMPARE(s.tempC, 26.5F);
        QCOMPARE(s.humidPct, 56.7F);
        QCOMPARE(s.voltage, 15.2F);
        QCOMPARE(s.distMm, 350.0F);
        QCOMPARE(s.boardTimeMs, 98765U);
        QCOMPARE(s.validMask,
                 quint8(kValidTempHum | kValidMpu | kValidVoltage | kValidDyp));

        // 长度不符拒绝
        QByteArray short1 = p.left(p.size() - 1);
        QVERIFY(!decodeSensorSummary(short1, s));

        // NaN 拒绝（不用异常值冒充有效数据）
        QByteArray nan;
        putF32(nan, std::numeric_limits<float>::quiet_NaN());
        for (int i = 0; i < 8; ++i) { putF32(nan, 1.0F); }
        putF32(nan, 1.0F);
        putF32(nan, 1.0F);
        nan.append(static_cast<char>(0x0FU));
        putU32(nan, 0U);
        QVERIFY(!decodeSensorSummary(nan, s));
    }

    void ackDecode()
    {
        QByteArray p;
        putU16(p, 3U);
        AckResult ack;
        QVERIFY(decodeAck(p, ack));
        QCOMPARE(ack.errCode, 3U);

        QByteArray bad;
        putU16(bad, 0U);
        bad.append(char(1));
        QVERIFY(!decodeAck(bad, ack));
    }

    void listDecodeRanges()
    {
        QByteArray angles;
        putU16(angles, 0U); putU16(angles, 90U); putU16(angles, 180U);
        std::vector<qint16> out;
        QVERIFY(decodeAngleList(angles, out));
        QCOMPARE(static_cast<int>(out.size()), 3);
        QCOMPARE(out[1], 90);

        QByteArray over;
        putU16(over, 181U);
        QVERIFY(!decodeAngleList(over, out));

        QByteArray props;
        putI16(props, -100); putI16(props, 100);
        QVERIFY(decodePropellerList(props, out));
        QCOMPARE(static_cast<int>(out.size()), 2);
        QCOMPARE(out[0], -100);

        QByteArray pover;
        putI16(pover, 101);
        QVERIFY(!decodePropellerList(pover, out));
    }
};

QTEST_APPLESS_MAIN(TestRegistry)
#include "test_registry.moc"
