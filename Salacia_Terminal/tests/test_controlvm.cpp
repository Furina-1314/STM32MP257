// ControlViewModel 单元测试：三态/限频合并/松手冲刷/基准切换/权限门控/紧急直发
#include <QtTest/qtest.h>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "communication/FunctionRegistry.h"
#include "communication/WireCodec.h"
#include "control/ControlViewModel.h"
#include "core/AppConfig.h"
#include "core/SafetyStateModel.h"

using namespace salacia;
using namespace salacia::wire;

namespace {

bool loadTestIni()
{
    static QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ctrl.ini"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(QByteArrayLiteral(
            "[control]\nservo_count = 10\nthruster_count = 6\n"
            "servo_min_deg = 0\nservo_max_deg = 180\nslider_rate_limit_ms = 50\n"
            "release_flush = true\n"
            "[ui]\nestop_confirm = false\nemergency_confirm = true\n"));
    file.close();
    return AppConfig::instance().load(path);
}

// 权限全开的 SafetyStateModel
void unlock(SafetyStateModel& safety)
{
    safety.setConnected(true);
    safety.applyAuthoritative(0U);
}

} // namespace

class TestControlVm : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(loadTestIni()); }

    void threeStateTracking();
    void rateLimitCoalesce();
    void releaseFlushImmediate();
    void permissionBlocks();
    void horizontalBaseSwitch();
    void baseSliderRejectedWhenOff();
    void estopDirectBypassQueue();
    void emergencyEmitted();
    void ackConfirmAndRejectRollback();
    void timeoutUnknownNoResend();
    void rangeClamp();

private:
    quint16 lastSeq_ = 100U;
    quint16 nextSeq() { return ++lastSeq_; }
};

void TestControlVm::threeStateTracking()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QCOMPARE(vm.servoCount(), 10);
    QCOMPARE(vm.thrusterCount(), 6);
    QCOMPARE(vm.servo(1).target, 90); // 初始回中
    QVERIFY(!vm.servo(1).confirmedValid); // 确认值未知（不用 0 冒充）

    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);
    vm.setServoTarget(1, 135, false);
    QTest::qWait(250); // 限频 50ms + 负载余量
    QCOMPARE(sendSpy.count(), 1);
    QCOMPARE(vm.servo(1).target, 135);
    QCOMPARE(vm.servo(1).sent, 135);
    QVERIFY(!vm.servo(1).confirmedValid);
}

void TestControlVm::rateLimitCoalesce()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);

    // 同通道 50ms 窗口内 5 次拖动 -> 只发最新值 1 帧
    vm.setThrusterTarget(2, 10, false);
    vm.setThrusterTarget(2, 30, false);
    vm.setThrusterTarget(2, 50, false);
    vm.setThrusterTarget(2, 70, false);
    vm.setThrusterTarget(2, 90, false);
    QTest::qWait(250); // 50ms 节拍 + 负载余量
    QCOMPARE(sendSpy.count(), 1);
    QCOMPARE(sendSpy.first().at(0).toUInt(),
             static_cast<uint>(static_cast<quint16>(Func::PropellerSet)));
    const QByteArray payload = sendSpy.first().at(1).toByteArray();
    bool ok = false;
    QCOMPARE(static_cast<quint8>(payload.at(0)), 2U);       // id=2
    QCOMPARE(getI16(payload, 1, ok), 90);                   // 最新值
    QVERIFY(ok);
    QCOMPARE(vm.thruster(2).sent, 90);
}

void TestControlVm::releaseFlushImmediate()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);
    vm.setServoTarget(3, 45, false);
    QTest::qWait(250); // 50ms 节拍 + 负载余量
    QCOMPARE(sendSpy.count(), 1);
    vm.setServoTarget(3, 120, true); // 松手：立即发最终值，不等节拍
    QCOMPARE(sendSpy.count(), 2);
    QCOMPARE(vm.servo(3).sent, 120);
}

void TestControlVm::permissionBlocks()
{
    SafetyStateModel safety;
    ControlViewModel vm(&safety); // 未连接
    QSignalSpy blockSpy(&vm, &ControlViewModel::permissionBlocked);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);
    QVERIFY(!vm.setServoTarget(1, 90, true));
    QVERIFY(!vm.setThrusterTarget(1, 50, true));
    QVERIFY(!vm.setBaseTarget(50, true));
    QCOMPARE(sendSpy.count(), 0);
    QCOMPARE(blockSpy.count(), 3);

    // safe on：全部禁用
    unlock(safety);
    safety.applyAuthoritative(wire::kStateSafe);
    QVERIFY(!vm.setServoTarget(1, 90, true));
    QCOMPARE(sendSpy.count(), 0);
}

void TestControlVm::horizontalBaseSwitch()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);

    // 逐路模式：推进器逐路可用，基准不可用
    vm.setThrusterTarget(1, 40, true);
    QCOMPARE(sendSpy.count(), 1);

    // 切 horizontal on：清空逐路待发 + 基准可用
    vm.setThrusterTarget(2, 60, false); // 留一条待发
    safety.applyAuthoritative(wire::kStateHorizontal);
    vm.onHorizontalChanged(true);
    QTest::qWait(250); // 50ms 节拍 + 负载余量
    // 隐藏控件的旧值不得发送（红线）：sendSpy 不得出现 id=2 的逐路帧
    for (const auto& args : sendSpy) {
        if (args.at(0).toUInt() == static_cast<uint>(static_cast<quint16>(Func::PropellerSet))) {
            const QByteArray payload = args.at(1).toByteArray();
            QVERIFY2(static_cast<quint8>(payload.at(0)) != 2U,
                     "hidden channel stale value must not be sent");
        }
    }

    // 基准滑条：单帧 6 路相同值
    QSignalSpy baseSpy(&vm, &ControlViewModel::baseUpdated);
    QVERIFY(vm.setBaseTarget(-35, true));
    QCOMPARE(sendSpy.count(), 2);
    const QByteArray basePayload = sendSpy.last().at(1).toByteArray();
    QCOMPARE(sendSpy.last().at(0).toUInt(),
             static_cast<uint>(static_cast<quint16>(Func::BaseValue)));
    QCOMPARE(basePayload.size(), 12);
    bool ok = false;
    for (int i = 0; i < 6; ++i) {
        QCOMPARE(getI16(basePayload, i * 2, ok), -35);
        QVERIFY(ok);
    }
    QVERIFY(baseSpy.count() >= 1);
}

void TestControlVm::baseSliderRejectedWhenOff()
{
    SafetyStateModel safety;
    unlock(safety); // horizontal off
    ControlViewModel vm(&safety);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);
    QVERIFY(!vm.setBaseTarget(50, true)); // 权限拒绝
    QCOMPARE(sendSpy.count(), 0);
}

void TestControlVm::estopDirectBypassQueue()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QSignalSpy estopSpy(&vm, &ControlViewModel::estopRequested);
    // 普通指令在队列中未冲刷时，estop 立即出队
    vm.setServoTarget(1, 100, false);
    vm.requestEstop();
    QCOMPARE(estopSpy.count(), 1);
    const QByteArray payload = estopSpy.first().at(0).toByteArray();
    QCOMPARE(payload.size(), 32); // 10+6 全零单帧
    QVERIFY(!vm.estopConfirmRequired()); // ini: estop_confirm=false

    // 断线后 estop 拒绝
    safety.setConnected(false);
    QSignalSpy estopSpy2(&vm, &ControlViewModel::estopRequested);
    vm.requestEstop();
    QCOMPARE(estopSpy2.count(), 0);
}

void TestControlVm::emergencyEmitted()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QVERIFY(vm.emergencyConfirmRequired()); // ini: emergency_confirm=true
    QSignalSpy emgSpy(&vm, &ControlViewModel::emergencyRequested);
    vm.requestEmergency();
    QCOMPARE(emgSpy.count(), 1);
}

void TestControlVm::ackConfirmAndRejectRollback()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);
    const quint16 seqA = nextSeq();
    const quint16 seqB = nextSeq();

    vm.setServoTarget(1, 120, true); // 立即发
    vm.onFrameSent(seqA, static_cast<quint16>(Func::ServoSet),
                   sendSpy.first().at(1).toByteArray());
    vm.onFrameAcked(seqA, 0U);
    QCOMPARE(vm.servo(1).confirmed, 120);
    QVERIFY(vm.servo(1).confirmedValid);

    // 被拒：目标回滚到最近确认值
    vm.setServoTarget(1, 170, true);
    vm.onFrameSent(seqB, static_cast<quint16>(Func::ServoSet),
                   sendSpy.last().at(1).toByteArray());
    vm.onFrameAcked(seqB, 2U); // errCode=2 拒绝
    QCOMPARE(vm.servo(1).target, 120); // 回滚
    QCOMPARE(vm.servo(1).confirmed, 120);
}

void TestControlVm::timeoutUnknownNoResend()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);
    QSignalSpy unknownSpy(&vm, &ControlViewModel::channelUnknown);
    const quint16 seq = nextSeq();
    vm.setThrusterTarget(4, 60, true);
    vm.onFrameSent(seq, static_cast<quint16>(Func::PropellerSet),
                   sendSpy.first().at(1).toByteArray());
    const int sentCount = sendSpy.count();
    vm.onFrameFailed(seq); // 超时
    QCOMPARE(unknownSpy.count(), 1);
    QCOMPARE(unknownSpy.first().at(0).toInt(), 1);  // 推进器
    QCOMPARE(unknownSpy.first().at(1).toInt(), 4);  // id=4
    QVERIFY(!vm.thruster(4).confirmedValid);
    QTest::qWait(250); // 50ms 节拍 + 负载余量
    QCOMPARE(sendSpy.count(), sentCount); // 不盲目自动重发
}

void TestControlVm::rangeClamp()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QVERIFY(vm.setServoTarget(1, 999, true));   // 钳到 180
    QCOMPARE(vm.servo(1).target, 180);
    QVERIFY(vm.setThrusterTarget(2, -500, true)); // 钳到 -100
    QCOMPARE(vm.thruster(2).target, -100);
    QVERIFY(!vm.setServoTarget(0, 90, true));  // id 越界
    QVERIFY(!vm.setServoTarget(11, 90, true));
}

QTEST_MAIN(TestControlVm)
#include "test_controlvm.moc"
