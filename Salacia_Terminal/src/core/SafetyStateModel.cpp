#include "SafetyStateModel.h"

#include <QDateTime>

namespace salacia {

namespace {

// 开关显示名（告警/日志文案用）
QString switchName(SwitchId id)
{
    switch (id) {
    case SwitchId::Safe:             return QString::fromLocal8Bit("Safe 安全保护");
    case SwitchId::AttitudeStab:     return QString::fromLocal8Bit("姿态稳定");
    case SwitchId::GlobalEnable:     return QString::fromLocal8Bit("推进器总使能");
    case SwitchId::VerticalEnable:   return QString::fromLocal8Bit("垂直推进使能");
    case SwitchId::HorizontalEnable: return QString::fromLocal8Bit("水平推进使能");
    case SwitchId::VerticalSync:     return QString::fromLocal8Bit("垂直同步");
    case SwitchId::HorizontalSync:   return QString::fromLocal8Bit("水平同步");
    default:                         return QStringLiteral("unknown");
    }
}

} // namespace

SafetyStateModel::SafetyStateModel(QObject* parent)
    : QObject(parent)
{
}

bool SafetyStateModel::switchFuncTarget(quint16 funcId, SwitchId& out, bool& toOn)
{
    switch (static_cast<wire::Func>(funcId)) {
    case wire::Func::SafeOn:             out = SwitchId::Safe; toOn = true; return true;
    case wire::Func::SafeOff:            out = SwitchId::Safe; toOn = false; return true;
    case wire::Func::HorizontalOn:       out = SwitchId::AttitudeStab; toOn = true; return true;
    case wire::Func::HorizontalOff:      out = SwitchId::AttitudeStab; toOn = false; return true;
    case wire::Func::MoveAll:            out = SwitchId::GlobalEnable; toOn = true; return true;
    case wire::Func::StopAll:            out = SwitchId::GlobalEnable; toOn = false; return true;
    case wire::Func::MoveVertical:       out = SwitchId::VerticalEnable; toOn = true; return true;
    case wire::Func::StopVertical:       out = SwitchId::VerticalEnable; toOn = false; return true;
    case wire::Func::MoveHorizontal:     out = SwitchId::HorizontalEnable; toOn = true; return true;
    case wire::Func::StopHorizontal:     out = SwitchId::HorizontalEnable; toOn = false; return true;
    case wire::Func::VerticalSyncOn:     out = SwitchId::VerticalSync; toOn = true; return true;
    case wire::Func::VerticalSyncOff:    out = SwitchId::VerticalSync; toOn = false; return true;
    case wire::Func::HorizontalSyncOn:   out = SwitchId::HorizontalSync; toOn = true; return true;
    case wire::Func::HorizontalSyncOff:  out = SwitchId::HorizontalSync; toOn = false; return true;
    default: return false; // estop/emergency/普通命令：不进入开关事务
    }
}

void SafetyStateModel::setConnected(bool on)
{
    if (connected_ == on) {
        return;
    }
    connected_ = on;
    if (!on) {
        // 断线：全部事务失效、权威回到未知（不重放红线配套语义）；
        // 回退到 Unknown 而非 Off（Unknown 不得显示为已关闭红线）
        for (PendingSwitchState& sw : switches_) {
            sw = PendingSwitchState{};
        }
        estopOn_ = false;
        emergencyOn_ = false;
        authorityKnown_ = false;
        illegalCombo_ = false;
        lastMaskV2_ = 0U;
    }
    emit stateChanged();
}

void SafetyStateModel::applyAuthoritativeV2(quint16 stateMask)
{
    lastMaskV2_ = stateMask;
    applySwitchAuthority(SwitchId::Safe,
                         (stateMask & wire::kStateV2Safe) != 0U);
    applySwitchAuthority(SwitchId::AttitudeStab,
                         (stateMask & wire::kStateV2AttitudeStab) != 0U);
    // stopped 位=1 表示停止锁存；使能开关 = 取反
    applySwitchAuthority(SwitchId::GlobalEnable,
                         (stateMask & wire::kStateV2GlobalStopped) == 0U);
    applySwitchAuthority(SwitchId::VerticalEnable,
                         (stateMask & wire::kStateV2VerticalStopped) == 0U);
    applySwitchAuthority(SwitchId::HorizontalEnable,
                         (stateMask & wire::kStateV2HorizontalStopped) == 0U);
    applySwitchAuthority(SwitchId::VerticalSync,
                         (stateMask & wire::kStateV2VerticalSync) != 0U);
    applySwitchAuthority(SwitchId::HorizontalSync,
                         (stateMask & wire::kStateV2HorizontalSync) != 0U);
    estopOn_ = (stateMask & wire::kStateV2Estop) != 0U;
    emergencyOn_ = (stateMask & wire::kStateV2Emergency) != 0U;
    authorityKnown_ = true;
    recheckIllegalCombo();
    emit stateChanged();
}

void SafetyStateModel::applyAuthoritative(quint8 legacyMask)
{
    // legacy 0x0102 仅含 4 位；使能/同步无事件依据，保持原状态不臆造
    applySwitchAuthority(SwitchId::Safe,
                         (legacyMask & wire::kStateSafe) != 0U);
    applySwitchAuthority(SwitchId::AttitudeStab,
                         (legacyMask & wire::kStateHorizontal) != 0U);
    estopOn_ = (legacyMask & wire::kStateEstop) != 0U;
    emergencyOn_ = (legacyMask & wire::kStateEmergency) != 0U;
    authorityKnown_ = true;
    recheckIllegalCombo();
    emit stateChanged();
}

void SafetyStateModel::applySwitchAuthority(SwitchId id, bool on)
{
    PendingSwitchState& sw = switches_[static_cast<int>(id)];
    if (sw.pending) {
        // StateEventV2 先于 ACK 到达：以事件为权威并清除对应 Pending（事实优先）
        sw.pending = false;
    } else if (sw.ackCommittedValid && (sw.ackCommittedValue != on)) {
        // 成功 ACK 提交后事件给出相反状态：协议状态不一致（高等级告警），
        // 以 StateEventV2 覆盖
        emit authorityConflict(QString::fromLocal8Bit(
                "协议状态不一致：成功 ACK 后状态事件报告 %1=%2（以状态事件为权威）")
                .arg(switchName(id),
                     on ? QString::fromLocal8Bit("ON")
                        : QString::fromLocal8Bit("OFF")));
    }
    sw.ackCommittedValid = false;
    sw.ackCommittedValue = false;
    sw.authoritative = on ? ModeState::On : ModeState::Off;
}

void SafetyStateModel::requestSent(quint16 seq, quint16 funcId)
{
    SwitchId id = SwitchId::Safe;
    bool toOn = false;
    if (!switchFuncTarget(funcId, id, toOn)) {
        return;
    }
    beginPending(id, toOn, seq);
    // Safe 单向联动：SafeOn 请求同时把姿态稳定带入 Pending 目标 ON
    //（Windows 只发一条业务级请求；A35 原子保证姿态稳定开启后才确认 Safe 成功）
    if ((id == SwitchId::Safe) && toOn
        && (switches_[static_cast<int>(SwitchId::AttitudeStab)].authoritative
                    != ModeState::On)) {
        beginPending(SwitchId::AttitudeStab, true, seq);
    }
    emit stateChanged();
}

void SafetyStateModel::beginPending(SwitchId id, bool toOn, quint16 seq)
{
    PendingSwitchState& sw = switches_[static_cast<int>(id)];
    sw.pending = true;
    sw.displayedTarget = toOn; // 立即显示目标状态（ProgressRing 由 UI 呈现）
    sw.pendingSeq = seq;
    sw.pendingDirection = toOn ? 1 : -1;
    sw.rollbackState = sw.authoritative; // 发起时权威值（回退目标）
    sw.pendingTimestampMs = QDateTime::currentMSecsSinceEpoch();
    sw.ackCommittedValid = false;
    sw.ackCommittedValue = false;
}

void SafetyStateModel::requestAcked(quint16 seq, quint16 funcId, quint16 errCode)
{
    SwitchId id = SwitchId::Safe;
    bool toOn = false;
    if (!switchFuncTarget(funcId, id, toOn)) {
        return; // estop/emergency：ACK 仅表示接受，激活事实由状态事件权威化
    }
    if (commitPendingBySeq(seq, errCode == 0U) && (errCode != 0U)) {
        emit modeRejected(funcId, errCode); // NACK 回退（UI 恢复 + 提示）
    }
    emit stateChanged();
}

void SafetyStateModel::requestFailed(quint16 seq, quint16 funcId)
{
    SwitchId id = SwitchId::Safe;
    bool toOn = false;
    if (!switchFuncTarget(funcId, id, toOn)) {
        return;
    }
    if (commitPendingBySeq(seq, false)) {
        emit switchTimeout(funcId); // 超时回退（UI 恢复 + 提示）
    }
    emit stateChanged();
}

bool SafetyStateModel::commitPendingBySeq(quint16 seq, bool success)
{
    bool consumed = false;
    for (PendingSwitchState& sw : switches_) {
        if (!sw.pending || (sw.pendingSeq != seq)) {
            continue;
        }
        sw.pending = false;
        if (success) {
            // 成功提交目标状态（含 SafeOn 联动的姿态稳定同 seq 提交）
            sw.authoritative = sw.displayedTarget ? ModeState::On : ModeState::Off;
            sw.ackCommittedValid = true;
            sw.ackCommittedValue = sw.displayedTarget;
        } else {
            // NACK/超时：回退发起前权威值（Unknown 保持 Unknown，不显示为 Off）
            sw.authoritative = sw.rollbackState;
        }
        consumed = true;
    }
    return consumed;
}

void SafetyStateModel::recheckIllegalCombo()
{
    const bool safeOn =
            (switches_[static_cast<int>(SwitchId::Safe)].authoritative
                     == ModeState::On);
    const bool stabOff =
            (switches_[static_cast<int>(SwitchId::AttitudeStab)].authoritative
                     == ModeState::Off);
    const bool illegal = safeOn && stabOff;
    if (illegal && !illegalCombo_) {
        illegalCombo_ = true;
        // 非法权威组合：锁定推进器控制 + 高等级告警，Windows 端不悄悄修正
        emit authorityConflict(QString::fromLocal8Bit(
                "非法权威状态：Safe=ON 且 姿态稳定=OFF（已锁定推进器控制，"
                "等待 A35 修正）"));
    } else if (!illegal && illegalCombo_) {
        illegalCombo_ = false; // A35 修正后解锁
    }
}

// ---------------------------------------------------------------- 权限矩阵

bool SafetyStateModel::canServoIndividual() const
{
    // 舵机权限红线：与 Safe/姿态稳定/同步/Stop-Move/Estop/Emergency 全部解耦，
    // 仅要求 TCP 连接存在（参数合法性与同通道 Pending 由 ControlViewModel 把关）
    return connected_;
}

bool SafetyStateModel::canThrusterGroup(bool vertical) const
{
    // 推进器组可操作 = TCP 已连接 + 权威状态已知 + 总使能 ON + 分组使能 ON +
    // 相关使能开关不处于 Pending（PendingToOff 同样禁用）+ 非非法权威组合
    if (!connected_ || !authorityKnown_ || illegalCombo_) {
        return false;
    }
    const PendingSwitchState& global =
            switches_[static_cast<int>(SwitchId::GlobalEnable)];
    const PendingSwitchState& group = switches_[static_cast<int>(
            vertical ? SwitchId::VerticalEnable : SwitchId::HorizontalEnable)];
    if ((global.authoritative != ModeState::On)
        || (group.authoritative != ModeState::On)) {
        return false;
    }
    return !global.pending && !group.pending;
}

bool SafetyStateModel::canThrusterIndividual() const
{
    // 逐路视图（Phase 16 分组 UI 落地前的兼容口径）：
    // 姿态稳定 OFF（逐路模式）且两组均可操作
    if (switches_[static_cast<int>(SwitchId::AttitudeStab)].authoritative
                != ModeState::Off) {
        return false;
    }
    return canThrusterGroup(true) && canThrusterGroup(false);
}

bool SafetyStateModel::canBaseSlider() const
{
    // 基准滑条：姿态稳定 ON（基准模式）且两组推进器均可提交
    //（Safe ON 不锁推进器提交，A35 限幅/拒绝由返回结果处理）
    if (switches_[static_cast<int>(SwitchId::AttitudeStab)].authoritative
                != ModeState::On) {
        return false;
    }
    return canThrusterGroup(true) && canThrusterGroup(false);
}

bool SafetyStateModel::baseSliderVisible() const
{
    return switches_[static_cast<int>(SwitchId::AttitudeStab)].authoritative
            == ModeState::On;
}

EmergencyButtonState SafetyStateModel::estopButton() const
{
    if (!connected_) {
        return EmergencyButtonState::Disabled; // 仅断线禁用（最高优先级安全通道）
    }
    if (estopOn_) {
        return EmergencyButtonState::Triggered; // 已确认：显示已触发
    }
    return EmergencyButtonState::Ready;
}

EmergencyButtonState SafetyStateModel::emergencyButton() const
{
    if (!connected_) {
        return EmergencyButtonState::Disabled;
    }
    if (emergencyOn_) {
        return EmergencyButtonState::InProgress; // 显示进行中
    }
    return EmergencyButtonState::Ready;
}

bool SafetyStateModel::controlsLocked() const
{
    return !connected_ || !authorityKnown_;
}

// ---------------------------------------------------------------- 布局判定

ThrusterGroupLayout SafetyStateModel::verticalLayout() const
{
    if (switches_[static_cast<int>(SwitchId::AttitudeStab)].authoritative
                == ModeState::On) {
        return ThrusterGroupLayout::Base;
    }
    if (switches_[static_cast<int>(SwitchId::VerticalSync)].authoritative
                == ModeState::On) {
        return ThrusterGroupLayout::Sync;
    }
    // 同步 Unknown 一律保守逐路（Unknown 不得显示为已关闭红线）
    return ThrusterGroupLayout::Individual;
}

ThrusterGroupLayout SafetyStateModel::horizontalLayout() const
{
    if (switches_[static_cast<int>(SwitchId::AttitudeStab)].authoritative
                == ModeState::On) {
        return ThrusterGroupLayout::Base;
    }
    if (switches_[static_cast<int>(SwitchId::HorizontalSync)].authoritative
                == ModeState::On) {
        return ThrusterGroupLayout::Sync;
    }
    return ThrusterGroupLayout::Individual;
}

// ---------------------------------------------------------------- 状态读取

ModeState SafetyStateModel::switchState(SwitchId id) const
{
    const PendingSwitchState& sw = switches_[static_cast<int>(id)];
    return sw.pending ? ModeState::Pending : sw.authoritative;
}

bool SafetyStateModel::switchPending(SwitchId id) const
{
    return switches_[static_cast<int>(id)].pending;
}

bool SafetyStateModel::switchDisplayedTarget(SwitchId id) const
{
    const PendingSwitchState& sw = switches_[static_cast<int>(id)];
    if (sw.pending) {
        return sw.displayedTarget;
    }
    return sw.authoritative == ModeState::On;
}

bool SafetyStateModel::switchToggleAllowed(SwitchId id, bool toOn) const
{
    // Pending 期间禁止重复点击同一开关
    if (switches_[static_cast<int>(id)].pending) {
        return false;
    }
    // Safe 单向联动红线：Safe ON 期间禁止关闭姿态稳定
    if ((id == SwitchId::AttitudeStab) && !toOn
        && (switches_[static_cast<int>(SwitchId::Safe)].authoritative
                    == ModeState::On)) {
        return false;
    }
    return true;
}

ModeState SafetyStateModel::safeState() const
{
    return switchState(SwitchId::Safe);
}

ModeState SafetyStateModel::horizontalState() const
{
    return switchState(SwitchId::AttitudeStab);
}

} // namespace salacia
