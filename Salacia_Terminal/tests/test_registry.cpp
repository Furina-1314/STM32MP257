// FunctionRegistry 单元测试：表完整性、执行器 ID 拓扑、值域校验、StateEventV2 解码
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
        // estop/emergency/stop-all 的 funcId/优先级严格分离；插队次序红线
        const FunctionEntry* estop = FunctionRegistry::findByName(
                QStringLiteral("estop"));
        const FunctionEntry* stop = FunctionRegistry::findByName(QStringLiteral("stop all"));
        const FunctionEntry* emg = FunctionRegistry::findByName(QStringLiteral("emergency"));
        QVERIFY(estop != nullptr);
        QVERIFY(stop != nullptr);
        QVERIFY(emg != nullptr);
        QVERIFY(estop->funcId != stop->funcId);
        QVERIFY(estop->funcId != emg->funcId);
        QCOMPARE(estop->priority, kPriorityEstop);
        QCOMPARE(emg->priority, kPriorityEmergency);
        QCOMPARE(stop->priority, kPriorityStopMove);
        QVERIFY(estop->priority < emg->priority);
        QVERIFY(emg->priority < stop->priority);
        QVERIFY(stop->priority < kPriorityNormal);

        // 二轮新增函数全部登记
        const quint16 newFuncs[] = {
            static_cast<quint16>(Func::MoveAll),
            static_cast<quint16>(Func::StopVertical),
            static_cast<quint16>(Func::MoveVertical),
            static_cast<quint16>(Func::StopHorizontal),
            static_cast<quint16>(Func::MoveHorizontal),
            static_cast<quint16>(Func::VerticalSyncOn),
            static_cast<quint16>(Func::VerticalSyncOff),
            static_cast<quint16>(Func::HorizontalSyncOn),
            static_cast<quint16>(Func::HorizontalSyncOff),
            static_cast<quint16>(Func::BaseValueVH),
            static_cast<quint16>(Func::StateEventV2),
        };
        for (const quint16 id : newFuncs) {
            QVERIFY2(FunctionRegistry::findByFuncId(id) != nullptr,
                     qPrintable(QStringLiteral("missing funcId 0x%1").arg(id, 4, 16, QLatin1Char('0'))));
        }
        // Stop/Move 组优先级 = kPriorityStopMove
        const quint16 stopMoveFuncs[] = {
            static_cast<quint16>(Func::StopAll),
            static_cast<quint16>(Func::MoveAll),
            static_cast<quint16>(Func::StopVertical),
            static_cast<quint16>(Func::MoveVertical),
            static_cast<quint16>(Func::StopHorizontal),
            static_cast<quint16>(Func::MoveHorizontal),
        };
        for (const quint16 id : stopMoveFuncs) {
            QCOMPARE(FunctionRegistry::findByFuncId(id)->priority, kPriorityStopMove);
        }
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

    void actuatorIdTopology()
    {
        // 舵机 wire 0..9
        QVERIFY(isValidServoId(0U));
        QVERIFY(isValidServoId(9U));
        QVERIFY(!isValidServoId(10U));
        QVERIFY(!isValidServoId(255U));
        // 垂直 wire 10..13 / 水平 wire 14..15
        QVERIFY(isVerticalThrusterId(10U));
        QVERIFY(isVerticalThrusterId(13U));
        QVERIFY(!isVerticalThrusterId(14U));
        QVERIFY(isHorizontalThrusterId(14U));
        QVERIFY(isHorizontalThrusterId(15U));
        QVERIFY(!isHorizontalThrusterId(13U));
        QVERIFY(isValidThrusterId(10U));
        QVERIFY(isValidThrusterId(15U));
        QVERIFY(!isValidThrusterId(9U));
        QVERIFY(!isValidThrusterId(16U));
        // UI 编号 <-> wireId 映射
        QCOMPARE(servoWireId(1), quint8(0U));
        QCOMPARE(servoWireId(10), quint8(9U));
        QCOMPARE(servoUiNumber(0U), 1);
        QCOMPARE(servoUiNumber(9U), 10);
        QCOMPARE(verticalWireId(1), quint8(10U));
        QCOMPARE(verticalWireId(4), quint8(13U));
        QCOMPARE(verticalUiNumber(13U), 4);
        QCOMPARE(horizontalWireId(1), quint8(14U));
        QCOMPARE(horizontalWireId(2), quint8(15U));
        QCOMPARE(horizontalUiNumber(15U), 2);
        // 扁平桥接：前 4 垂直、后 2 水平
        QCOMPARE(thrusterWireIdFromFlat(1), quint8(10U));
        QCOMPARE(thrusterWireIdFromFlat(4), quint8(13U));
        QCOMPARE(thrusterWireIdFromFlat(5), quint8(14U));
        QCOMPARE(thrusterWireIdFromFlat(6), quint8(15U));
        QCOMPARE(thrusterFlatFromWireId(10U), 1);
        QCOMPARE(thrusterFlatFromWireId(15U), 6);
        QCOMPARE(kServoCount, 10);
        QCOMPARE(kVerticalCount, 4);
        QCOMPARE(kHorizontalCount, 2);
        QCOMPARE(kThrusterCount, 6);
    }

    void servoSetValueRange()
    {
        ServoSetCmd cmd;
        cmd.id = 2U; // wire 2 = 舵机3（UI）
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

        // 非法 ID 拒绝（wire 0..9 之外；广播不适用于 set）
        cmd.angleDeg = 90U;
        cmd.id = 10U;
        QVERIFY(encodeServoSet(cmd).isEmpty());
        cmd.id = 255U;
        QVERIFY(encodeServoSet(cmd).isEmpty());
        cmd.id = 9U;
        QVERIFY(!encodeServoSet(cmd).isEmpty());
    }

    void propellerSetValueRange()
    {
        PropellerSetCmd cmd;
        cmd.id = 12U; // wire 12 = 垂直3（UI）
        cmd.valuePct = -80;
        const QByteArray ok = encodePropellerSet(cmd);
        QCOMPARE(ok.size(), 3);
        bool flag = true;
        QCOMPARE(static_cast<quint8>(ok.at(0)), 12U);
        QCOMPARE(getI16(ok, 1, flag), -80);
        QVERIFY(flag);

        cmd.valuePct = 101;
        QVERIFY(encodePropellerSet(cmd).isEmpty());
        cmd.valuePct = -101;
        QVERIFY(encodePropellerSet(cmd).isEmpty());
        cmd.valuePct = 100;
        QVERIFY(!encodePropellerSet(cmd).isEmpty());

        // 非法 ID 拒绝（wire 10..15 之外；广播不适用于 set）
        cmd.valuePct = 0;
        cmd.id = 9U;
        QVERIFY(encodePropellerSet(cmd).isEmpty());
        cmd.id = 16U;
        QVERIFY(encodePropellerSet(cmd).isEmpty());
        cmd.id = 0U;
        QVERIFY(encodePropellerSet(cmd).isEmpty());
        cmd.id = 255U;
        QVERIFY(encodePropellerSet(cmd).isEmpty());
        cmd.id = 10U;
        QVERIFY(!encodePropellerSet(cmd).isEmpty());
        cmd.id = 15U;
        QVERIFY(!encodePropellerSet(cmd).isEmpty());

        // mid/stop 类允许广播
        QVERIFY(!encodeServoMid(kIdBroadcast).isEmpty());
        QVERIFY(encodeServoMid(10U).isEmpty());
        QVERIFY(!encodeServoMid(9U).isEmpty());
        QVERIFY(!encodePropellerStop(kIdBroadcast).isEmpty());
        QVERIFY(encodePropellerStop(9U).isEmpty());
        QVERIFY(encodePropellerStop(16U).isEmpty());
        QVERIFY(!encodePropellerStop(15U).isEmpty());
    }

    void baseValueVhFrame()
    {
        const QByteArray payload = encodeBaseValueVH(-35, 40);
        QCOMPARE(payload.size(), 4); // 2×i16：垂直基准、水平基准
        bool flag = true;
        QCOMPARE(getI16(payload, 0, flag), -35);
        QCOMPARE(getI16(payload, 2, flag), 40);
        QVERIFY(flag);
        QVERIFY(encodeBaseValueVH(101, 0).isEmpty());
        QVERIFY(encodeBaseValueVH(0, -101).isEmpty());
    }

    void stateEventV2Decode()
    {
        // u8 version(=2) + u16 mask（小端）
        QByteArray p;
        p.append(static_cast<char>(kStateEventV2Version));
        putU16(p, quint16(kStateV2Safe | kStateV2VerticalSync | kStateV2Emergency));
        quint16 mask = 0U;
        QVERIFY(decodeStateEventV2(p, mask));
        QCOMPARE(mask, quint16(kStateV2Safe | kStateV2VerticalSync | kStateV2Emergency));

        // 版本不符整帧拒绝（禁止静默按旧位义解读）
        QByteArray badVer;
        badVer.append(char(1));
        putU16(badVer, quint16(kStateV2Safe));
        QVERIFY(!decodeStateEventV2(badVer, mask));

        // 未知位整帧拒绝
        QByteArray badBits;
        badBits.append(static_cast<char>(kStateEventV2Version));
        putU16(badBits, quint16(0x0200U)); // bit9 未定义
        QVERIFY(!decodeStateEventV2(badBits, mask));

        // 长度不符拒绝
        QVERIFY(!decodeStateEventV2(p.left(2), mask));
        QVERIFY(!decodeStateEventV2(QByteArray(), mask));

        // legacy 0x0102 仍可解码且位义不变
        QByteArray legacy;
        legacy.append(static_cast<char>(kStateSafe | kStateEstop));
        quint8 legacyMask = 0U;
        QVERIFY(decodeStateEvent(legacy, legacyMask));
        QCOMPARE(legacyMask, quint8(kStateSafe | kStateEstop));

        // 全部已定义位可解码
        QByteArray full;
        full.append(static_cast<char>(kStateEventV2Version));
        putU16(full, kStateV2KnownMask);
        QVERIFY(decodeStateEventV2(full, mask));
        QCOMPARE(mask, kStateV2KnownMask);
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
