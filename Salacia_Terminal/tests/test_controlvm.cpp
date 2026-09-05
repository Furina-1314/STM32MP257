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

// 权限全开的 SafetyStateModel（V2 全清：全使能 ON、稳定/同步 OFF、无紧急）
void unlock(SafetyStateModel& safety)
{
    safety.setConnected(true);
    safety.applyAuthoritativeV2(0U);
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
    void groupApiAndWireMapping();
    void dualBaseValues();
    void stopMoveRequests();
    void noReplayAfterRelatch();

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
    QCOMPARE(static_cast<quint8>(payload.at(0)), 11U);     // 扁平 2 -> wire 11（垂直2）
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

    // safe on（姿态稳定联动 ON）：舵机仍可操作（解耦红线）；
    // 姿态稳定 ON -> 逐路禁用、基准可用
    unlock(safety);
    safety.applyAuthoritativeV2(quint16(wire::kStateV2Safe | wire::kStateV2AttitudeStab));
    QSignalSpy sendSpy2(&vm, &ControlViewModel::sendRequested);
    QVERIFY(vm.setServoTarget(1, 90, true));     // safe 不锁舵机
    QVERIFY(!vm.setThrusterTarget(1, 50, true)); // 姿态稳定 ON：逐路禁用
    QVERIFY(vm.setBaseTarget(50, true));         // 基准可用
    QCOMPARE(sendSpy2.count(), 2);               // 舵机 + 基准各 1 帧
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

    // 切姿态稳定 on：清空逐路待发 + 基准可用
    vm.setThrusterTarget(2, 60, false); // 留一条待发
    safety.applyAuthoritativeV2(quint16(wire::kStateV2AttitudeStab));
    vm.onAuthorityStateChanged();
    QTest::qWait(250); // 50ms 节拍 + 负载余量
    // 隐藏控件的旧值不得发送（红线）：sendSpy 不得出现 wire 11（扁平 2）的逐路帧
    for (const auto& args : sendSpy) {
        if (args.at(0).toUInt() == static_cast<uint>(static_cast<quint16>(Func::PropellerSet))) {
            const QByteArray payload = args.at(1).toByteArray();
            QVERIFY2(static_cast<quint8>(payload.at(0)) != 11U,
                     "hidden channel stale value must not be sent");
        }
    }

    // 基准滑条：BaseValueVH 单帧 2×i16（Phase 15 拆双基准前双组同值）
    QSignalSpy baseSpy(&vm, &ControlViewModel::baseUpdated);
    QVERIFY(vm.setBaseTarget(-35, true));
    QCOMPARE(sendSpy.count(), 2);
    const QByteArray basePayload = sendSpy.last().at(1).toByteArray();
    QCOMPARE(sendSpy.last().at(0).toUInt(),
             static_cast<uint>(static_cast<quint16>(Func::BaseValueVH)));
    QCOMPARE(basePayload.size(), 4);
    bool ok = false;
    QCOMPARE(getI16(basePayload, 0, ok), -35);
    QCOMPARE(getI16(basePayload, 2, ok), -35);
    QVERIFY(ok);
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
    QCOMPARE(payload.size(), 0); // 空载荷：仅推进器置零，不含舵机角度
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

void TestControlVm::groupApiAndWireMapping()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QCOMPARE(vm.servoCount(), 10);
    QCOMPARE(vm.verticalCount(), 4);
    QCOMPARE(vm.horizontalCount(), 2);
    QCOMPARE(vm.thrusterCount(), 6);

    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);
    QVERIFY(vm.setVerticalThrusterTarget(3, 60, true));    // 垂直3 -> wire 12
    QVERIFY(vm.setHorizontalThrusterTarget(2, -40, true)); // 水平2 -> wire 15
    QCOMPARE(sendSpy.count(), 2);
    QCOMPARE(static_cast<quint8>(sendSpy.at(0).at(1).toByteArray().at(0)), 12U);
    QCOMPARE(static_cast<quint8>(sendSpy.at(1).at(1).toByteArray().at(0)), 15U);
    QCOMPARE(vm.verticalThruster(3).target, 60);
    QCOMPARE(vm.horizontalThruster(2).target, -40);
    QCOMPARE(vm.thruster(3).target, 60);  // 扁平桥接：垂直3 = 扁平3
    QCOMPARE(vm.thruster(6).target, -40); // 水平2 = 扁平6（wire 15）
    // 组内编号越界拒绝
    QVERIFY(!vm.setVerticalThrusterTarget(5, 10, true));
    QVERIFY(!vm.setHorizontalThrusterTarget(3, 10, true));
}

void TestControlVm::dualBaseValues()
{
    SafetyStateModel safety;
    unlock(safety);
    safety.applyAuthoritativeV2(quint16(wire::kStateV2AttitudeStab)); // 基准模式
    ControlViewModel vm(&safety);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);

    QVERIFY(vm.setVerticalBaseTarget(-30, true)); // 松手：立即单帧双值
    QCOMPARE(sendSpy.count(), 1);
    const QByteArray p1 = sendSpy.at(0).at(1).toByteArray();
    QCOMPARE(sendSpy.at(0).at(0).toUInt(),
             static_cast<uint>(static_cast<quint16>(Func::BaseValueVH)));
    QCOMPARE(p1.size(), 4);
    bool ok = false;
    QCOMPARE(getI16(p1, 0, ok), -30); // 垂直基准
    QCOMPARE(getI16(p1, 2, ok), 0);   // 水平基准保持原值
    QVERIFY(ok);

    QVERIFY(vm.setHorizontalBaseTarget(45, true));
    QCOMPARE(sendSpy.count(), 2);
    const QByteArray p2 = sendSpy.at(1).at(1).toByteArray();
    QCOMPARE(getI16(p2, 0, ok), -30);
    QCOMPARE(getI16(p2, 2, ok), 45);
    QVERIFY(ok);
    QCOMPARE(vm.verticalBaseTarget(), -30);
    QCOMPARE(vm.horizontalBaseTarget(), 45);

    // ACK 确认：垂直组确认 -30、水平组确认 45（分组各自回填）
    const quint16 seq = nextSeq();
    vm.onFrameSent(seq, static_cast<quint16>(Func::BaseValueVH), p2);
    vm.onFrameAcked(seq, 0U);
    QCOMPARE(vm.verticalThruster(1).confirmed, -30);
    QCOMPARE(vm.verticalThruster(4).confirmed, -30);
    QCOMPARE(vm.horizontalThruster(1).confirmed, 45);
    QCOMPARE(vm.horizontalThruster(2).confirmed, 45);
    QVERIFY(vm.verticalThruster(1).confirmedValid);
    QVERIFY(vm.horizontalThruster(2).confirmedValid);
}

void TestControlVm::stopMoveRequests()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);
    QSignalSpy blockSpy(&vm, &ControlViewModel::permissionBlocked);

    vm.requestStopAll();
    vm.requestMoveAll();
    vm.requestStopGroup(true);
    vm.requestStopGroup(false);
    vm.requestMoveGroup(true);
    vm.requestMoveGroup(false);
    QCOMPARE(sendSpy.count(), 6);
    QCOMPARE(blockSpy.count(), 0);
    QCOMPARE(sendSpy.at(0).at(0).toUInt(), static_cast<uint>(static_cast<quint16>(Func::StopAll)));
    QCOMPARE(sendSpy.at(1).at(0).toUInt(), static_cast<uint>(static_cast<quint16>(Func::MoveAll)));
    QCOMPARE(sendSpy.at(2).at(0).toUInt(), static_cast<uint>(static_cast<quint16>(Func::StopVertical)));
    QCOMPARE(sendSpy.at(3).at(0).toUInt(), static_cast<uint>(static_cast<quint16>(Func::StopHorizontal)));
    QCOMPARE(sendSpy.at(4).at(0).toUInt(), static_cast<uint>(static_cast<quint16>(Func::MoveVertical)));
    QCOMPARE(sendSpy.at(5).at(0).toUInt(), static_cast<uint>(static_cast<quint16>(Func::MoveHorizontal)));
    for (const auto& args : sendSpy) {
        QVERIFY(args.at(1).toByteArray().isEmpty()); // 全部空载荷
    }

    // 总使能 OFF（停止锁存）：分组 Move 禁止，Stop 方向不受限
    safety.applyAuthoritativeV2(wire::kStateV2GlobalStopped);
    const int before = sendSpy.count();
    vm.requestMoveGroup(true);
    QCOMPARE(sendSpy.count(), before);
    QVERIFY(blockSpy.count() >= 1);
    vm.requestStopGroup(true);
    QCOMPARE(sendSpy.count(), before + 1); // Stop 允许

    // 总使能重新 ON：分组 Move 恢复
    safety.applyAuthoritativeV2(0U);
    vm.requestMoveGroup(true);
    QCOMPARE(sendSpy.count(), before + 2);

    // 断线：全部 Stop/Move 拒绝
    safety.setConnected(false);
    const int before2 = sendSpy.count();
    vm.requestStopAll();
    vm.requestMoveAll();
    vm.requestStopGroup(false);
    vm.requestMoveGroup(false);
    QCOMPARE(sendSpy.count(), before2);
}

void TestControlVm::noReplayAfterRelatch()
{
    SafetyStateModel safety;
    unlock(safety);
    ControlViewModel vm(&safety);
    QSignalSpy sendSpy(&vm, &ControlViewModel::sendRequested);

    // 垂直组留待发（未松手）后进入停止锁存：待发被清空，不得稍后发送
    vm.setThrusterTarget(2, 70, false);
    safety.applyAuthoritativeV2(wire::kStateV2VerticalStopped);
    vm.onAuthorityStateChanged();
    QTest::qWait(250); // 50ms 节拍 + 负载余量
    for (const auto& args : sendSpy) {
        if (args.at(0).toUInt() == static_cast<uint>(static_cast<quint16>(Func::PropellerSet))) {
            QVERIFY2(static_cast<quint8>(args.at(1).toByteArray().at(0)) != 11U,
                     "latched group stale value must not be sent");
        }
    }
    QCOMPARE(vm.thruster(2).target, 70); // 目标显示保留（UI 层），但不重放

    // 垂直组重新使能：不自动恢复或重发旧推进器目标
    safety.applyAuthoritativeV2(0U);
    vm.onAuthorityStateChanged();
    QTest::qWait(250);
    for (const auto& args : sendSpy) {
        if (args.at(0).toUInt() == static_cast<uint>(static_cast<quint16>(Func::PropellerSet))) {
            QVERIFY2(static_cast<quint8>(args.at(1).toByteArray().at(0)) != 11U,
                     "re-enabled group must not replay old target");
        }
    }
}

QTEST_MAIN(TestControlVm)
#include "test_controlvm.moc"
