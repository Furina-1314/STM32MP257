// SafetyStateModel 单元测试：PendingSwitchState 事务 / Safe 联动 / 权限矩阵 /
// 布局判定 / 非法权威组合 / StateEventV2 冲突
#include <QSignalSpy>
#include <QtTest/qtest.h>

#include "communication/FunctionRegistry.h"
#include "communication/WireConstants.h"
#include "core/SafetyStateModel.h"

using namespace salacia;
using namespace salacia::wire;

namespace {
constexpr quint16 kFuncSafeOn = static_cast<quint16>(Func::SafeOn);
constexpr quint16 kFuncSafeOff = static_cast<quint16>(Func::SafeOff);
constexpr quint16 kFuncStabOn = static_cast<quint16>(Func::HorizontalOn);
constexpr quint16 kFuncStabOff = static_cast<quint16>(Func::HorizontalOff);
constexpr quint16 kFuncMoveAll = static_cast<quint16>(Func::MoveAll);
constexpr quint16 kFuncStopAll = static_cast<quint16>(Func::StopAll);
constexpr quint16 kFuncMoveVertical = static_cast<quint16>(Func::MoveVertical);
constexpr quint16 kFuncVertSyncOn = static_cast<quint16>(Func::VerticalSyncOn);
constexpr quint16 kFuncEstop = static_cast<quint16>(Func::Estop);
constexpr quint16 kFuncServoSet = static_cast<quint16>(Func::ServoSet);
} // namespace

class TestSafetyState : public QObject
{
    Q_OBJECT

private slots:
    // ---- 基础权限 ----
    void disconnectedEverythingLocked();
    void connectedBeforeAuthority();
    void v2AllClearManual();
    void attitudeStabOnRow();
    void safeOnDoesNotLockAnything();
    void illegalSafeWithoutStab();
    void stopMoveLatchMatrix();
    void estopEmergencyButtons();

    // ---- 开关事务 ----
    void pendingToOnAndOff();
    void safeOnLinkagePending();
    void safeOnFailRollsBackBoth();
    void ackFailRollsBack();
    void timeoutRollsBack();
    void disconnectResetsToUnknown();
    void stateEventClearsPending();
    void ackThenConflictingEvent();
    void toggleBlockedWhilePendingOrSafe();
    void nonSwitchFuncIgnored();

    // ---- 联动与布局 ----
    void safeOffKeepsStab();
    void syncLayouts();
    void legacyFallback();
};

// ---------------------------------------------------------------- 基础权限

void TestSafetyState::disconnectedEverythingLocked()
{
    SafetyStateModel s;
    QVERIFY(!s.canServoIndividual());
    QVERIFY(!s.canThrusterIndividual());
    QVERIFY(!s.canBaseSlider());
    QCOMPARE(s.estopButton(), EmergencyButtonState::Disabled);
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::Disabled);
    QVERIFY(s.controlsLocked());
    QCOMPARE(s.switchState(SwitchId::Safe), ModeState::Unknown);
    QCOMPARE(s.switchState(SwitchId::GlobalEnable), ModeState::Unknown);
}

void TestSafetyState::connectedBeforeAuthority()
{
    SafetyStateModel s;
    s.setConnected(true);
    // 舵机权限红线：仅要求 TCP 连接（权威未知不锁舵机）
    QVERIFY(s.canServoIndividual());
    // 推进器：权威未知保守禁用
    QVERIFY(!s.canThrusterIndividual());
    QVERIFY(!s.canBaseSlider());
    QVERIFY(s.controlsLocked());
    // Unknown 不得显示为已关闭
    QCOMPARE(s.switchState(SwitchId::Safe), ModeState::Unknown);
    QCOMPARE(s.switchState(SwitchId::VerticalSync), ModeState::Unknown);
    // 紧急按钮仅断线禁用
    QCOMPARE(s.estopButton(), EmergencyButtonState::Ready);
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::Ready);
}

void TestSafetyState::v2AllClearManual()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(0U); // 全关/全使能/同步关/无紧急
    QVERIFY(s.authorityKnown());
    QVERIFY(s.canServoIndividual());
    QVERIFY(s.canThrusterIndividual());
    QVERIFY(!s.canBaseSlider());
    QVERIFY(!s.baseSliderVisible());
    QVERIFY(!s.controlsLocked());
    QCOMPARE(s.safeState(), ModeState::Off);
    QCOMPARE(s.horizontalState(), ModeState::Off);
    QCOMPARE(s.switchState(SwitchId::GlobalEnable), ModeState::On);
    QCOMPARE(s.switchState(SwitchId::VerticalEnable), ModeState::On);
    QCOMPARE(s.switchState(SwitchId::HorizontalEnable), ModeState::On);
    QCOMPARE(s.switchState(SwitchId::VerticalSync), ModeState::Off);
    QCOMPARE(s.verticalLayout(), ThrusterGroupLayout::Individual);
    QCOMPARE(s.horizontalLayout(), ThrusterGroupLayout::Individual);
}

void TestSafetyState::attitudeStabOnRow()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(kStateV2AttitudeStab);
    // 姿态稳定 ON：基准模式；舵机不受影响（解耦红线）
    QVERIFY(s.canServoIndividual());
    QVERIFY(!s.canThrusterIndividual());
    QVERIFY(s.canBaseSlider());
    QVERIFY(s.baseSliderVisible());
    QCOMPARE(s.verticalLayout(), ThrusterGroupLayout::Base);
    QCOMPARE(s.horizontalLayout(), ThrusterGroupLayout::Base);
}

void TestSafetyState::safeOnDoesNotLockAnything()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(quint16(kStateV2Safe | kStateV2AttitudeStab));
    // Safe ON（合法：姿态稳定同步 ON）：舵机可操作、推进器仍可提交
    QVERIFY(s.canServoIndividual());
    QVERIFY(s.canBaseSlider());
    QVERIFY(s.canThrusterGroup(true));
    QVERIFY(s.canThrusterGroup(false));
    QVERIFY(!s.thrustersLockedByAuthority());
    // Safe ON 期间禁止关闭姿态稳定（单向联动）
    QVERIFY(!s.switchToggleAllowed(SwitchId::AttitudeStab, false));
    QVERIFY(s.switchToggleAllowed(SwitchId::AttitudeStab, true));
    QVERIFY(s.switchToggleAllowed(SwitchId::Safe, false));
}

void TestSafetyState::illegalSafeWithoutStab()
{
    SafetyStateModel s;
    QSignalSpy conflictSpy(&s, &SafetyStateModel::authorityConflict);
    s.setConnected(true);
    s.applyAuthoritativeV2(kStateV2Safe); // Safe ON + 姿态稳定 OFF：非法
    QCOMPARE(conflictSpy.count(), 1);
    QVERIFY(s.thrustersLockedByAuthority());
    QVERIFY(!s.canThrusterIndividual());
    QVERIFY(!s.canBaseSlider());
    QVERIFY(!s.canThrusterGroup(true));
    QVERIFY(!s.canThrusterGroup(false));
    QVERIFY(s.canServoIndividual()); // 舵机仍可操作
    // A35 修正（姿态稳定补开）后解锁，不再重复告警
    s.applyAuthoritativeV2(quint16(kStateV2Safe | kStateV2AttitudeStab));
    QCOMPARE(conflictSpy.count(), 1);
    QVERIFY(!s.thrustersLockedByAuthority());
    QVERIFY(s.canBaseSlider());
}

void TestSafetyState::stopMoveLatchMatrix()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(0U);
    // 全局停止锁存（总使能 OFF）：两组全禁，分组开关保留权威显示
    s.applyAuthoritativeV2(kStateV2GlobalStopped);
    QCOMPARE(s.switchState(SwitchId::GlobalEnable), ModeState::Off);
    QVERIFY(!s.canThrusterGroup(true));
    QVERIFY(!s.canThrusterGroup(false));
    QVERIFY(!s.canThrusterIndividual());
    QVERIFY(s.canServoIndividual()); // 舵机不受 Stop-Move 影响
    // 仅垂直组停止：只禁垂直组
    s.applyAuthoritativeV2(kStateV2VerticalStopped);
    QVERIFY(!s.canThrusterGroup(true));
    QVERIFY(s.canThrusterGroup(false));
    QVERIFY(s.canServoIndividual());
    // 姿态稳定 ON + 垂直停止：基准滑条因垂直组锁存不可用
    s.applyAuthoritativeV2(quint16(kStateV2AttitudeStab | kStateV2VerticalStopped));
    QVERIFY(!s.canBaseSlider());
    QVERIFY(!s.canThrusterGroup(true));
    QVERIFY(s.canThrusterGroup(false));
    // 恢复
    s.applyAuthoritativeV2(kStateV2AttitudeStab);
    QVERIFY(s.canBaseSlider());
}

void TestSafetyState::estopEmergencyButtons()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(kStateV2Estop);
    QVERIFY(s.estopActive());
    QCOMPARE(s.estopButton(), EmergencyButtonState::Triggered);
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::Ready);
    s.applyAuthoritativeV2(quint16(kStateV2Estop | kStateV2Emergency | kStateV2GlobalStopped));
    QVERIFY(s.emergencyActive());
    QCOMPARE(s.emergencyButton(), EmergencyButtonState::InProgress);
    QVERIFY(!s.canThrusterGroup(true)); // estop 置零并停止（globalStopped 锁存）
    QVERIFY(s.canServoIndividual());
}

// ---------------------------------------------------------------- 开关事务

void TestSafetyState::pendingToOnAndOff()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(0U);
    // PendingToOn
    s.requestSent(1U, kFuncStabOn);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::Pending);
    QVERIFY(s.switchPending(SwitchId::AttitudeStab));
    QVERIFY(s.switchDisplayedTarget(SwitchId::AttitudeStab)); // 立即显示目标 ON
    QVERIFY(!s.switchToggleAllowed(SwitchId::AttitudeStab, false)); // Pending 禁重复点击
    s.requestAcked(1U, kFuncStabOn, 0U);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::On);
    QVERIFY(!s.switchPending(SwitchId::AttitudeStab));
    // PendingToOff
    s.requestSent(2U, kFuncStabOff);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::Pending);
    QVERIFY(!s.switchDisplayedTarget(SwitchId::AttitudeStab));
    s.requestAcked(2U, kFuncStabOff, 0U);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::Off);
    // 使能开关事务同规则
    s.requestSent(3U, kFuncStopAll);
    QCOMPARE(s.switchState(SwitchId::GlobalEnable), ModeState::Pending);
    QVERIFY(!s.switchDisplayedTarget(SwitchId::GlobalEnable)); // 目标 OFF
    s.requestAcked(3U, kFuncStopAll, 0U);
    QCOMPARE(s.switchState(SwitchId::GlobalEnable), ModeState::Off);
    s.requestSent(4U, kFuncMoveAll);
    s.requestAcked(4U, kFuncMoveAll, 0U);
    QCOMPARE(s.switchState(SwitchId::GlobalEnable), ModeState::On);
    // 同步开关
    s.requestSent(5U, kFuncVertSyncOn);
    QCOMPARE(s.switchState(SwitchId::VerticalSync), ModeState::Pending);
    s.requestAcked(5U, kFuncVertSyncOn, 0U);
    QCOMPARE(s.switchState(SwitchId::VerticalSync), ModeState::On);
    QCOMPARE(s.verticalLayout(), ThrusterGroupLayout::Sync);
}

void TestSafetyState::safeOnLinkagePending()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(0U);
    QSignalSpy spy(&s, &SafetyStateModel::stateChanged);
    s.requestSent(11U, kFuncSafeOn);
    // Safe 与姿态稳定同时进入 Pending 目标 ON（双 Ring）
    QCOMPARE(s.switchState(SwitchId::Safe), ModeState::Pending);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::Pending);
    QVERIFY(s.switchDisplayedTarget(SwitchId::AttitudeStab));
    // 成功：双 ON
    s.requestAcked(11U, kFuncSafeOn, 0U);
    QCOMPARE(s.switchState(SwitchId::Safe), ModeState::On);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::On);
    QVERIFY(spy.count() >= 2);
}

void TestSafetyState::safeOnFailRollsBackBoth()
{
    SafetyStateModel s;
    QSignalSpy rejectSpy(&s, &SafetyStateModel::modeRejected);
    s.setConnected(true);
    s.applyAuthoritativeV2(0U); // 双 Off 权威
    s.requestSent(12U, kFuncSafeOn);
    s.requestAcked(12U, kFuncSafeOn, 3U); // NACK
    QCOMPARE(s.switchState(SwitchId::Safe), ModeState::Off);       // 双回退
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::Off);
    QCOMPARE(rejectSpy.count(), 1);
}

void TestSafetyState::ackFailRollsBack()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(kStateV2AttitudeStab); // 权威 On
    s.requestSent(21U, kFuncStabOff);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::Pending);
    QVERIFY(!s.switchDisplayedTarget(SwitchId::AttitudeStab)); // 显示目标 OFF
    s.requestAcked(21U, kFuncStabOff, 7U);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::On); // 回退权威值
}

void TestSafetyState::timeoutRollsBack()
{
    SafetyStateModel s;
    QSignalSpy timeoutSpy(&s, &SafetyStateModel::switchTimeout);
    s.setConnected(true);
    s.applyAuthoritativeV2(0U);
    s.requestSent(31U, kFuncMoveVertical);
    QCOMPARE(s.switchState(SwitchId::VerticalEnable), ModeState::Pending);
    s.requestFailed(31U, kFuncMoveVertical);
    QCOMPARE(s.switchState(SwitchId::VerticalEnable), ModeState::On); // 回退
    QCOMPARE(timeoutSpy.count(), 1);
}

void TestSafetyState::disconnectResetsToUnknown()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(quint16(kStateV2Safe | kStateV2AttitudeStab));
    QCOMPARE(s.safeState(), ModeState::On);
    s.setConnected(false);
    QCOMPARE(s.safeState(), ModeState::Unknown); // 断线 -> Unknown（非 Off）
    QCOMPARE(s.horizontalState(), ModeState::Unknown);
    QCOMPARE(s.switchState(SwitchId::GlobalEnable), ModeState::Unknown);
    QVERIFY(!s.authorityKnown());
    QVERIFY(!s.canThrusterGroup(true));
    QVERIFY(s.controlsLocked());
}

void TestSafetyState::stateEventClearsPending()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(0U);
    s.requestSent(41U, kFuncStabOn);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::Pending);
    // 权威事件先于 ACK 到达：事件为权威并清除 Pending
    s.applyAuthoritativeV2(kStateV2AttitudeStab);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::On);
    QVERIFY(!s.switchPending(SwitchId::AttitudeStab));
    // 晚到的成功 ACK 不再改变状态
    s.requestAcked(41U, kFuncStabOn, 0U);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::On);
}

void TestSafetyState::ackThenConflictingEvent()
{
    SafetyStateModel s;
    QSignalSpy conflictSpy(&s, &SafetyStateModel::authorityConflict);
    s.setConnected(true);
    s.applyAuthoritativeV2(0U);
    s.requestSent(51U, kFuncStabOn);
    s.requestAcked(51U, kFuncStabOn, 0U); // 成功 ACK 提交 ON
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::On);
    // 之后 StateEvent 报告相反状态：事件覆盖 + 协议不一致告警
    s.applyAuthoritativeV2(0U);
    QCOMPARE(conflictSpy.count(), 1);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::Off); // 事件覆盖
}

void TestSafetyState::toggleBlockedWhilePendingOrSafe()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(0U);
    s.requestSent(61U, kFuncVertSyncOn);
    QVERIFY(!s.switchToggleAllowed(SwitchId::VerticalSync, true));
    QVERIFY(!s.switchToggleAllowed(SwitchId::VerticalSync, false));
    s.requestAcked(61U, kFuncVertSyncOn, 0U);
    QVERIFY(s.switchToggleAllowed(SwitchId::VerticalSync, false));
    // Safe ON 期间禁关姿态稳定
    s.applyAuthoritativeV2(quint16(kStateV2Safe | kStateV2AttitudeStab));
    QVERIFY(!s.switchToggleAllowed(SwitchId::AttitudeStab, false));
}

void TestSafetyState::nonSwitchFuncIgnored()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(0U);
    s.requestSent(71U, kFuncServoSet); // 普通命令：不进入开关事务
    QCOMPARE(s.safeState(), ModeState::Off);
    s.requestAcked(71U, kFuncServoSet, 0U);
    QCOMPARE(s.safeState(), ModeState::Off);
    QCOMPARE(s.horizontalState(), ModeState::Off);
    // estop/emergency：ACK 不改变开关（激活由状态事件权威化）
    s.requestSent(72U, kFuncEstop);
    QCOMPARE(s.switchState(SwitchId::Safe), ModeState::Off);
    s.requestAcked(72U, kFuncEstop, 0U);
    QVERIFY(!s.estopActive());
}

// ---------------------------------------------------------------- 联动与布局

void TestSafetyState::safeOffKeepsStab()
{
    SafetyStateModel s;
    s.setConnected(true);
    s.applyAuthoritativeV2(quint16(kStateV2Safe | kStateV2AttitudeStab));
    s.requestSent(81U, kFuncSafeOff);
    QCOMPARE(s.switchState(SwitchId::Safe), ModeState::Pending);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::On); // 姿态稳定不动
    s.requestAcked(81U, kFuncSafeOff, 0U);
    QCOMPARE(s.switchState(SwitchId::Safe), ModeState::Off);
    QCOMPARE(s.switchState(SwitchId::AttitudeStab), ModeState::On); // 保持 ON
    QVERIFY(s.switchToggleAllowed(SwitchId::AttitudeStab, false));  // 恢复可操作
}

void TestSafetyState::syncLayouts()
{
    SafetyStateModel s;
    s.setConnected(true);
    // 姿态稳定 OFF：各组按自身同步状态决定布局
    s.applyAuthoritativeV2(quint16(kStateV2VerticalSync | kStateV2HorizontalSync));
    QCOMPARE(s.verticalLayout(), ThrusterGroupLayout::Sync);
    QCOMPARE(s.horizontalLayout(), ThrusterGroupLayout::Sync);
    s.applyAuthoritativeV2(kStateV2VerticalSync); // 仅垂直同步
    QCOMPARE(s.verticalLayout(), ThrusterGroupLayout::Sync);
    QCOMPARE(s.horizontalLayout(), ThrusterGroupLayout::Individual);
    s.applyAuthoritativeV2(kStateV2HorizontalSync); // 仅水平同步
    QCOMPARE(s.verticalLayout(), ThrusterGroupLayout::Individual);
    QCOMPARE(s.horizontalLayout(), ThrusterGroupLayout::Sync);
    s.applyAuthoritativeV2(0U); // 双同步 OFF
    QCOMPARE(s.verticalLayout(), ThrusterGroupLayout::Individual);
    QCOMPARE(s.horizontalLayout(), ThrusterGroupLayout::Individual);
    // 姿态稳定 ON：两组均为基准布局（同步状态仍可切换但布局不变）
    s.applyAuthoritativeV2(quint16(kStateV2AttitudeStab | kStateV2VerticalSync));
    QCOMPARE(s.verticalLayout(), ThrusterGroupLayout::Base);
    QCOMPARE(s.horizontalLayout(), ThrusterGroupLayout::Base);
    QCOMPARE(s.switchState(SwitchId::VerticalSync), ModeState::On); // 权威显示保留
}

void TestSafetyState::legacyFallback()
{
    SafetyStateModel s;
    s.setConnected(true);
    // legacy 0x0102：仅 4 位；使能/同步保持 Unknown（推进器保守禁用）
    s.applyAuthoritative(quint8(kStateSafe | kStateHorizontal));
    QCOMPARE(s.safeState(), ModeState::On);
    QCOMPARE(s.horizontalState(), ModeState::On);
    QVERIFY(s.canServoIndividual());
    QVERIFY(!s.canThrusterIndividual()); // 使能未知保守禁用
    QVERIFY(s.authorityKnown());
    // estop 位
    s.applyAuthoritative(quint8(kStateEstop));
    QVERIFY(s.estopActive());
    QCOMPARE(s.safeState(), ModeState::Off);
}

QTEST_APPLESS_MAIN(TestSafetyState)
#include "test_safetystate.moc"
