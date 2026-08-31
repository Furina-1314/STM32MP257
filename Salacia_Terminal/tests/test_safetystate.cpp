// SafetyStateModel 单元测试：权限矩阵全行 / 三态转移 / 回滚 / 断线语义
#include <QtTest/qtest.h>

#include "communication/FunctionRegistry.h"
#include "communication/WireConstants.h"
#include "core/SafetyStateModel.h"

using namespace salacia;
using namespace salacia::wire;

namespace {
constexpr quint16 kFuncServoSet = static_cast<quint16>(Func::ServoSet);
constexpr quint16 kFuncSafeOn = static_cast<quint16>(Func::SafeOn);
constexpr quint16 kFuncSafeOff = static_cast<quint16>(Func::SafeOff);
constexpr quint16 kFuncHorizOn = static_cast<quint16>(Func::HorizontalOn);
constexpr quint16 kFuncHorizOff = static_cast<quint16>(Func::HorizontalOff);
constexpr quint16 kFuncEstop = static_cast<quint16>(Func::Estop);
constexpr quint16 kFuncEmergency = static_cast<quint16>(Func::Emergency);
} // namespace

class TestSafetyState : public QObject
{
    Q_OBJECT

private slots:
    void disconnectedEverythingLocked();
    void normalManualAllEnabled();
    void horizontalOnRow();
    void safeOnRow();
    void estopConfirmedRow();
    void emergencyConfirmedRow();
    void pendingTriState();
    void ackFailRollsBack();
    void timeoutRestoresAuthoritative();
    void disconnectResetsToUnknown();
    void stateEventClearsPending();
    void nonModeFuncIgnored();
};

void TestSafetyState::disconnectedEverythingLocked()
{
    SafetyStateModel s;
    // 矩阵行 6：断线/状态未知
    QVERIFY(!s.canServoIndividual());
    QVERIFY(!s.canThrusterIndividual());
    QVERIFY(!s.canBaseSlider());
    QCOMPARE(s.estopButton(), EmergencyButtonState::Disabled);
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::Disabled);
    QVERIFY(s.controlsLocked());
}

void TestSafetyState::normalManualAllEnabled()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(0U); // 全部关闭 + 权威已知
    // 矩阵行 1：普通手动
    QVERIFY(s.canServoIndividual());
    QVERIFY(s.canThrusterIndividual());
    QVERIFY(!s.canBaseSlider());       // 基准滑条隐藏/禁用
    QVERIFY(!s.baseSliderVisible());
    QCOMPARE(s.estopButton(), EmergencyButtonState::Ready);
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::Ready);
    QVERIFY(!s.controlsLocked());
}

void TestSafetyState::horizontalOnRow()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(kStateHorizontal);
    // 矩阵行 2：horizontal on 且非 safe
    QVERIFY(!s.canServoIndividual());  // 舵机置灰禁用
    QVERIFY(!s.canThrusterIndividual());
    QVERIFY(s.canBaseSlider());        // 基准滑条可用
    QVERIFY(s.baseSliderVisible());
    QCOMPARE(s.estopButton(), EmergencyButtonState::Ready);
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::Ready);
}

void TestSafetyState::safeOnRow()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(kStateSafe | kStateHorizontal);
    // 矩阵行 3：safe on 全部普通控制禁用
    QVERIFY(!s.canServoIndividual());
    QVERIFY(!s.canThrusterIndividual());
    QVERIFY(!s.canBaseSlider());
    QVERIFY(s.controlsLocked());
    QCOMPARE(s.estopButton(), EmergencyButtonState::Ready); // 紧急仍可用
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::Ready);
}

void TestSafetyState::estopConfirmedRow()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(kStateEstop);
    // 矩阵行 4：estop 已确认
    QVERIFY(!s.canServoIndividual());
    QVERIFY(!s.canBaseSlider());
    QCOMPARE(s.estopButton(), EmergencyButtonState::Triggered); // 显示已触发
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::Ready); // 按对端状态显示
}

void TestSafetyState::emergencyConfirmedRow()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(kStateEmergency);
    // 矩阵行 5：emergency 已确认
    QVERIFY(!s.canServoIndividual());
    QVERIFY(!s.canBaseSlider());
    QCOMPARE(s.estopButton(), EmergencyButtonState::Ready);     // 仍可用
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::InProgress); // 显示进行中
}

void TestSafetyState::pendingTriState()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(0U);
    QCOMPARE(s.safeState(), ModeState::Off);
    s.requestSent(1U, kFuncSafeOn);
    QCOMPARE(s.safeState(), ModeState::Pending); // 请求中：不当作成功
    QVERIFY(!s.canServoIndividual());            // Pending 保守禁用
    s.requestAcked(1U, kFuncSafeOn, 0U);
    QCOMPARE(s.safeState(), ModeState::On);
}

void TestSafetyState::ackFailRollsBack()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(0U); // safe=Off 权威
    s.requestSent(5U, kFuncSafeOn);
    s.requestAcked(5U, kFuncSafeOn, 3U); // 错误码 3 = 拒绝
    QCOMPARE(s.safeState(), ModeState::Off); // 回滚权威值
    QVERIFY(s.canServoIndividual());
}

void TestSafetyState::timeoutRestoresAuthoritative()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(0U);
    s.requestSent(7U, kFuncHorizOn);
    QCOMPARE(s.horizontalState(), ModeState::Pending);
    s.requestFailed(7U, kFuncHorizOn);
    QCOMPARE(s.horizontalState(), ModeState::Off); // 超时恢复权威值
}

void TestSafetyState::disconnectResetsToUnknown()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(kStateSafe);
    QCOMPARE(s.safeState(), ModeState::On);
    s.setConnected(false);
    QCOMPARE(s.safeState(), ModeState::Unknown);   // 断线 -> 未知
    QCOMPARE(s.horizontalState(), ModeState::Unknown);
    QVERIFY(!s.authorityKnown());
    QVERIFY(s.controlsLocked());
}

void TestSafetyState::stateEventClearsPending()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(0U);
    s.requestSent(9U, kFuncHorizOn);
    QCOMPARE(s.horizontalState(), ModeState::Pending);
    s.applyAuthoritative(kStateHorizontal); // 板端权威事件先于 ACK 到达
    QCOMPARE(s.horizontalState(), ModeState::On); // 事实优先于请求状态
    QVERIFY(s.canBaseSlider());
}

void TestSafetyState::nonModeFuncIgnored()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritative(0U);
    s.requestSent(11U, kFuncServoSet); // 非模式命令：不进入 Pending
    QCOMPARE(s.safeState(), ModeState::Off);
    s.requestAcked(11U, kFuncServoSet, 0U); // 不影响模式
    QCOMPARE(s.safeState(), ModeState::Off);
    QCOMPARE(s.horizontalState(), ModeState::Off);
}

QTEST_APPLESS_MAIN(TestSafetyState)
#include "test_safetystate.moc"
