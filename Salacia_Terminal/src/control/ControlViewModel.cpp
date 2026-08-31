#include "ControlViewModel.h"

#include <QDateTime>

#include "core/AppConfig.h"
#include "core/Logger.h"

namespace salacia {

ControlViewModel::ControlViewModel(SafetyStateModel* safety, QObject* parent)
    : QObject(parent)
    , safety_(safety)
{
    const AppConfig& cfg = AppConfig::instance();
    servoMinDeg_ = cfg.servoMinDeg();
    servoMaxDeg_ = cfg.servoMaxDeg();
    servoMidDeg_ = (servoMinDeg_ + servoMaxDeg_) / 2;
    thrusterMinPct_ = cfg.thrusterMinPct();
    thrusterMaxPct_ = cfg.thrusterMaxPct();
    estopConfirm_ = cfg.estopConfirmEnabled();
    emergencyConfirm_ = cfg.emergencyConfirmEnabled();
    releaseFlush_ = cfg.releaseFlush();

    servos_.resize(cfg.servoCount());
    thrusters_.resize(cfg.thrusterCount());
    for (ChannelVm& ch : servos_) {
        ch.target = servoMidDeg_; // 初始目标=回中（确认值未知）
    }
    for (ChannelVm& ch : thrusters_) {
        ch.target = 0;
    }

    flushTimer_.setInterval(cfg.sliderRateLimitMs());
    connect(&flushTimer_, &QTimer::timeout, this, &ControlViewModel::flushPending);
    flushTimer_.start();
}

ChannelVm ControlViewModel::servo(int id) const
{
    if ((id >= 1) && (id <= servos_.size())) {
        return servos_.at(id - 1);
    }
    return ChannelVm{};
}

ChannelVm ControlViewModel::thruster(int id) const
{
    if ((id >= 1) && (id <= thrusters_.size())) {
        return thrusters_.at(id - 1);
    }
    return ChannelVm{};
}

bool ControlViewModel::checkServoAllowed()
{
    if ((safety_ != nullptr) && !safety_->canServoIndividual()) {
        emit permissionBlocked(
                QString::fromLocal8Bit("当前模式下舵机逐路控制不可用（safe/estop/emergency/断线）"));
        return false;
    }
    return true;
}

bool ControlViewModel::checkThrusterAllowed()
{
    if ((safety_ != nullptr) && !safety_->canThrusterIndividual()) {
        emit permissionBlocked(
                QString::fromLocal8Bit("当前模式下推进器逐路控制不可用（horizontal on 走基准滑条，"
                                       "safe/estop/emergency/断线为禁用）"));
        return false;
    }
    return true;
}

bool ControlViewModel::checkBaseAllowed()
{
    if ((safety_ != nullptr) && !safety_->canBaseSlider()) {
        emit permissionBlocked(
                QString::fromLocal8Bit("基准滑条仅在 horizontal on 且非 safe 时可用"));
        return false;
    }
    return true;
}

bool ControlViewModel::setServoTarget(int id, int deg, bool released)
{
    if ((id < 1) || (id > servos_.size())) {
        return false;
    }
    if (!checkServoAllowed()) {
        return false;
    }
    if (deg < servoMinDeg_) {
        deg = servoMinDeg_;
    }
    if (deg > servoMaxDeg_) {
        deg = servoMaxDeg_;
    }
    servos_[id - 1].target = deg;
    if (released && releaseFlush_) {
        pendingServo_.remove(id);
        sendServoNow(id); // 松手立即发最终值
    } else {
        pendingServo_.insert(id, deg);
    }
    emit channelUpdated(0, id);
    return true;
}

bool ControlViewModel::setThrusterTarget(int id, int pct, bool released)
{
    if ((id < 1) || (id > thrusters_.size())) {
        return false;
    }
    if (!checkThrusterAllowed()) {
        return false;
    }
    if (pct < thrusterMinPct_) {
        pct = thrusterMinPct_;
    }
    if (pct > thrusterMaxPct_) {
        pct = thrusterMaxPct_;
    }
    thrusters_[id - 1].target = pct;
    if (released && releaseFlush_) {
        pendingThruster_.remove(id);
        sendThrusterNow(id);
    } else {
        pendingThruster_.insert(id, pct);
    }
    emit channelUpdated(1, id);
    return true;
}

bool ControlViewModel::setBaseTarget(int pct, bool released)
{
    if (!checkBaseAllowed()) {
        return false;
    }
    if (pct < thrusterMinPct_) {
        pct = thrusterMinPct_;
    }
    if (pct > thrusterMaxPct_) {
        pct = thrusterMaxPct_;
    }
    baseTarget_ = pct;
    if (released && releaseFlush_) {
        basePending_ = false;
        sendBaseNow();
    } else {
        basePending_ = true;
    }
    emit baseUpdated();
    return true;
}

void ControlViewModel::servoMid(int id)
{
    setServoTarget(id, servoMidDeg_, true);
}

void ControlViewModel::allThrustersNeutral()
{
    for (int id = 1; id <= thrusters_.size(); ++id) {
        setThrusterTarget(id, 0, false);
    }
    flushPending(); // 中位=安全动作，立即冲刷
}

void ControlViewModel::requestEstop()
{
    if ((safety_ != nullptr)
        && (safety_->estopButton() == EmergencyButtonState::Disabled)) {
        emit permissionBlocked(QString::fromLocal8Bit("链路不可用，紧急停机无法下发"));
        return;
    }
    // 单帧 10+6 零值，绕过合并节拍；TcpClient 紧急队列保证最高优先级
    emit estopRequested(wire::encodeEstop(servoCount(), thrusterCount()));
}

void ControlViewModel::requestEmergency()
{
    if ((safety_ != nullptr)
        && (safety_->emergencyButton() == EmergencyButtonState::Disabled)) {
        emit permissionBlocked(QString::fromLocal8Bit("链路不可用，紧急上浮无法下发"));
        return;
    }
    emit emergencyRequested();
}

void ControlViewModel::sendServoNow(int id)
{
    wire::ServoSetCmd cmd;
    cmd.id = static_cast<quint8>(id);
    cmd.angleDeg = static_cast<quint16>(servos_.at(id - 1).target);
    const QByteArray payload = wire::encodeServoSet(cmd);
    if (payload.isEmpty()) {
        return; // 值域校验失败（编码器拒绝）
    }
    servos_[id - 1].sent = servos_.at(id - 1).target;
    emit sendRequested(static_cast<quint16>(wire::Func::ServoSet), payload);
}

void ControlViewModel::sendThrusterNow(int id)
{
    wire::PropellerSetCmd cmd;
    cmd.id = static_cast<quint8>(id);
    cmd.valuePct = static_cast<qint16>(thrusters_.at(id - 1).target);
    const QByteArray payload = wire::encodePropellerSet(cmd);
    if (payload.isEmpty()) {
        return;
    }
    thrusters_[id - 1].sent = thrusters_.at(id - 1).target;
    emit sendRequested(static_cast<quint16>(wire::Func::PropellerSet), payload);
}

void ControlViewModel::sendBaseNow()
{
    const QByteArray payload = wire::encodeBaseValue(thrusterCount(),
                                                     static_cast<qint16>(baseTarget_));
    if (payload.isEmpty()) {
        return;
    }
    for (ChannelVm& ch : thrusters_) {
        ch.sent = baseTarget_; // 基准值即全部推进器的已发送值
    }
    emit sendRequested(static_cast<quint16>(wire::Func::BaseValue), payload);
}

void ControlViewModel::flushPending()
{
    // 限频合并：每通道只发最新值（陈旧值被覆盖）
    const QMap<int, int> servos = pendingServo_;
    pendingServo_.clear();
    for (auto it = servos.constBegin(); it != servos.constEnd(); ++it) {
        sendServoNow(it.key());
    }
    const QMap<int, int> thrusters = pendingThruster_;
    pendingThruster_.clear();
    for (auto it = thrusters.constBegin(); it != thrusters.constEnd(); ++it) {
        sendThrusterNow(it.key());
    }
    if (basePending_) {
        basePending_ = false;
        sendBaseNow();
    }
}

void ControlViewModel::onFrameSent(quint16 seq, quint16 funcId,
                                   const QByteArray& payload)
{
    SentTrack track;
    if (funcId == static_cast<quint16>(wire::Func::ServoSet)) {
        track.kind = 0;
        track.id = payload.isEmpty() ? 0 : static_cast<int>(static_cast<quint8>(payload.at(0)));
        track.value = servos_.value(track.id - 1).sent;
    } else if (funcId == static_cast<quint16>(wire::Func::PropellerSet)) {
        track.kind = 1;
        track.id = payload.isEmpty() ? 0 : static_cast<int>(static_cast<quint8>(payload.at(0)));
        track.value = thrusters_.value(track.id - 1).sent;
    } else if (funcId == static_cast<quint16>(wire::Func::BaseValue)) {
        track.kind = 2;
        track.value = baseTarget_;
    } else {
        return;
    }
    inFlight_.insert(seq, track);
}

void ControlViewModel::onFrameAcked(quint16 seq, quint16 errCode)
{
    const SentTrack track = inFlight_.take(seq);
    if (track.kind == 0) {
        if ((track.id >= 1) && (track.id <= servos_.size())) {
            if (errCode == 0U) {
                servos_[track.id - 1].confirmed = track.value;
                servos_[track.id - 1].confirmedValid = true;
            } else {
                // 被拒：控件恢复最近确认值（权限矩阵配套红线）
                servos_[track.id - 1].target = servos_.at(track.id - 1).confirmed;
            }
            emit channelUpdated(0, track.id);
        }
    } else if (track.kind == 1) {
        if ((track.id >= 1) && (track.id <= thrusters_.size())) {
            if (errCode == 0U) {
                thrusters_[track.id - 1].confirmed = track.value;
                thrusters_[track.id - 1].confirmedValid = true;
            } else {
                thrusters_[track.id - 1].target = thrusters_.at(track.id - 1).confirmed;
            }
            emit channelUpdated(1, track.id);
        }
    } else if (track.kind == 2) {
        if (errCode == 0U) {
            for (ChannelVm& ch : thrusters_) {
                ch.confirmed = track.value;
                ch.confirmedValid = true;
            }
        } else {
            for (ChannelVm& ch : thrusters_) {
                ch.target = ch.confirmed;
            }
        }
        emit baseUpdated();
    }
}

void ControlViewModel::onFrameFailed(quint16 seq)
{
    const SentTrack track = inFlight_.take(seq);
    // 超时：显示状态未知，不自动重发（超时红线）
    if (track.kind == 0) {
        emit channelUnknown(0, track.id);
    } else if (track.kind == 1) {
        emit channelUnknown(1, track.id);
    } else if (track.kind == 2) {
        for (int id = 1; id <= thrusters_.size(); ++id) {
            emit channelUnknown(1, id);
        }
    }
}

void ControlViewModel::onHorizontalChanged(bool on)
{
    if (on) {
        // 切基准模式：清空逐路待发（隐藏控件不得稍后发送旧值红线）
        pendingThruster_.clear();
    }
    // 关闭基准模式恢复逐路：无待发恢复动作，由 UI 重新驱动
}

} // namespace salacia
