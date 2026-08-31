#include "SafetyStateModel.h"

#include <QDateTime>
#include <QMap>

namespace salacia {

SafetyStateModel::SafetyStateModel(QObject* parent)
    : QObject(parent)
{
}

bool SafetyStateModel::modeFunc(quint16 funcId)
{
    switch (static_cast<wire::Func>(funcId)) {
    case wire::Func::SafeOn:
    case wire::Func::SafeOff:
    case wire::Func::HorizontalOn:
    case wire::Func::HorizontalOff:
    case wire::Func::Estop:
    case wire::Func::Emergency:
    case wire::Func::Stop:
        return true;
    default:
        return false;
    }
}

void SafetyStateModel::setConnected(bool on)
{
    if (connected_ == on) {
        return;
    }
    connected_ = on;
    if (!on) {
        // 断线：挂起请求全部失效，模式回到未知（不重放红线配套语义）
        pending_.clear();
        safe_ = ModeState::Unknown;
        horizontal_ = ModeState::Unknown;
        authorityKnown_ = false;
    }
    emit stateChanged();
}

void SafetyStateModel::applyAuthoritative(quint8 stateMask)
{
    lastMask_ = stateMask;
    estopOn_ = (stateMask & wire::kStateEstop) != 0U;
    emergencyOn_ = (stateMask & wire::kStateEmergency) != 0U;
    // 权威事件到达时清除对应模式的 Pending（板端已给出事实）
    if (safe_ == ModeState::Pending) {
        safe_ = ModeState::Unknown;
    }
    if (horizontal_ == ModeState::Pending) {
        horizontal_ = ModeState::Unknown;
    }
    safe_ = ((stateMask & wire::kStateSafe) != 0U) ? ModeState::On : ModeState::Off;
    horizontal_ = ((stateMask & wire::kStateHorizontal) != 0U) ? ModeState::On
                                                               : ModeState::Off;
    authorityKnown_ = true;
    emit stateChanged();
}

void SafetyStateModel::requestSent(quint16 seq, quint16 funcId)
{
    if (!modeFunc(funcId)) {
        return;
    }
    PendingRequest req;
    req.funcId = funcId;
    req.atMs = QDateTime::currentMSecsSinceEpoch();
    pending_.insert(seq, req);

    // 点击仅进入"请求中"，不当作成功（状态红线）
    switch (static_cast<wire::Func>(funcId)) {
    case wire::Func::SafeOn:
    case wire::Func::SafeOff:
        safe_ = ModeState::Pending;
        break;
    case wire::Func::HorizontalOn:
    case wire::Func::HorizontalOff:
        horizontal_ = ModeState::Pending;
        break;
    default:
        break; // estop/emergency/stop 的确认由 StateEvent 驱动
    }
    emit stateChanged();
}

void SafetyStateModel::requestAcked(quint16 seq, quint16 funcId, quint16 errCode)
{
    if (!modeFunc(funcId)) {
        return;
    }
    pending_.remove(seq);
    applyModeResult(funcId, errCode == 0U);
    if (errCode != 0U) {
        emit modeRejected(funcId, errCode);
    }
    emit stateChanged();
}

void SafetyStateModel::requestFailed(quint16 seq, quint16 funcId)
{
    if (!modeFunc(funcId)) {
        return;
    }
    if (!pending_.contains(seq)) {
        return;
    }
    pending_.remove(seq);
    // 失败/超时：恢复最近权威值（未知则保持未知，不保留 Pending）
    applyModeResult(funcId, false);
    emit stateChanged();
}

void SafetyStateModel::applyModeResult(quint16 funcId, bool success)
{
    const auto func = static_cast<wire::Func>(funcId);
    if (!success) {
        // 回滚权威值
        safe_ = ((lastMask_ & wire::kStateSafe) != 0U) ? ModeState::On : ModeState::Off;
        horizontal_ = ((lastMask_ & wire::kStateHorizontal) != 0U) ? ModeState::On
                                                                   : ModeState::Off;
        return;
    }
    switch (func) {
    case wire::Func::SafeOn:
        safe_ = ModeState::On;
        break;
    case wire::Func::SafeOff:
        safe_ = ModeState::Off;
        break;
    case wire::Func::HorizontalOn:
        horizontal_ = ModeState::On;
        break;
    case wire::Func::HorizontalOff:
        horizontal_ = ModeState::Off;
        break;
    default:
        break; // estop/emergency：ACK 表示接受，激活事实仍由 StateEvent 权威化
    }
}

// ---------------------------------------------------------------- 权限矩阵

bool SafetyStateModel::canServoIndividual() const
{
    // 行 1 可用；行 2（horizontal on）舵机置灰禁用；行 3/4/5/6 禁用。
    // safe 必须为权威 Off：Pending/Unknown 一律保守禁用
    return connected_ && authorityKnown_ && (safe_ == ModeState::Off)
            && (horizontal_ == ModeState::Off) && !estopOn_ && !emergencyOn_;
}

bool SafetyStateModel::canThrusterIndividual() const
{
    // 同舵机（canServoIndividual 已含 horizontal off）
    return canServoIndividual();
}

bool SafetyStateModel::canBaseSlider() const
{
    // 仅行 2：horizontal on 且非 safe/estop/emergency
    return connected_ && authorityKnown_ && (safe_ == ModeState::Off)
            && (horizontal_ == ModeState::On) && !estopOn_ && !emergencyOn_;
}

bool SafetyStateModel::baseSliderVisible() const
{
    return (horizontal_ == ModeState::On);
}

EmergencyButtonState SafetyStateModel::estopButton() const
{
    if (!connected_ || !authorityKnown_) {
        return EmergencyButtonState::Disabled; // 断线/未知：无法下发
    }
    if (estopOn_) {
        return EmergencyButtonState::Triggered; // 已确认：显示已触发
    }
    if (emergencyOn_) {
        return EmergencyButtonState::Ready; // emergency 进行中 estop 仍可用（矩阵行 5）
    }
    return EmergencyButtonState::Ready;
}

EmergencyButtonState SafetyStateModel::emergencyButton() const
{
    if (!connected_ || !authorityKnown_) {
        return EmergencyButtonState::Disabled;
    }
    if (emergencyOn_) {
        return EmergencyButtonState::InProgress; // 显示进行中（矩阵行 5）
    }
    if (estopOn_) {
        return EmergencyButtonState::Ready; // estop 后按对端状态显示（矩阵行 4）
    }
    return EmergencyButtonState::Ready;
}

bool SafetyStateModel::controlsLocked() const
{
    return !canServoIndividual();
}

} // namespace salacia
