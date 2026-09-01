#include "ControlViewModel.h"

#include <QDateTime>

#include "communication/WireCodec.h"
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

    // 通道拓扑以 WireConstants 为唯一权威：舵机 10（wire 0..9）、
    // 垂直 4（CH10-13）、水平 2（CH14-15）
    servos_.resize(wire::kServoCount);
    thrusters_.resize(wire::kThrusterCount);
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

// ---------------------------------------------------------------- 通道访问

ChannelVm ControlViewModel::servo(int uiNumber) const
{
    if ((uiNumber >= 1) && (uiNumber <= servos_.size())) {
        return servos_.at(uiNumber - 1);
    }
    return ChannelVm{};
}

ChannelVm ControlViewModel::verticalThruster(int uiNumber) const
{
    return thruster(uiNumber); // 扁平 1..4 = 垂直组
}

ChannelVm ControlViewModel::horizontalThruster(int uiNumber) const
{
    return thruster(wire::kVerticalCount + uiNumber); // 扁平 5..6 = 水平组
}

ChannelVm ControlViewModel::thruster(int flat) const
{
    if ((flat >= 1) && (flat <= thrusters_.size())) {
        return thrusters_.at(flat - 1);
    }
    return ChannelVm{};
}

// ---------------------------------------------------------------- 权限门控

bool ControlViewModel::checkServoAllowed()
{
    if ((safety_ != nullptr) && !safety_->canServoIndividual()) {
        emit permissionBlocked(
                QString::fromLocal8Bit("链路不可用，舵机控制无法下发（断线）"));
        return false;
    }
    return true;
}

bool ControlViewModel::checkThrusterGroupAllowed(bool vertical)
{
    if (safety_ == nullptr) {
        return true;
    }
    // 逐路控制仅在姿态稳定 OFF（逐路模式）时可用；基准模式走基准滑条
    if (safety_->switchState(SwitchId::AttitudeStab) != ModeState::Off) {
        emit permissionBlocked(
                QString::fromLocal8Bit("推进器逐路控制不可用（姿态稳定开启走基准滑条，"
                                       "或使能请求中）"));
        return false;
    }
    if (!safety_->canThrusterGroup(vertical)) {
        emit permissionBlocked(vertical
                ? QString::fromLocal8Bit("垂直推进组不可用（使能开关非 ON/请求中，"
                                         "或权威状态未知/非法，或链路异常）")
                : QString::fromLocal8Bit("水平推进组不可用（使能开关非 ON/请求中，"
                                         "或权威状态未知/非法，或链路异常）"));
        return false;
    }
    return true;
}

bool ControlViewModel::checkBaseAllowed()
{
    if ((safety_ != nullptr) && !safety_->canBaseSlider()) {
        emit permissionBlocked(
                QString::fromLocal8Bit("基准滑条仅在姿态稳定开启且推进器使能可用时可用"));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- UI 写入口

bool ControlViewModel::setServoTarget(int uiNumber, int deg, bool released)
{
    if ((uiNumber < 1) || (uiNumber > servos_.size())) {
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
    servos_[uiNumber - 1].target = deg;
    if (released && releaseFlush_) {
        pendingServo_.remove(uiNumber);
        sendServoNow(uiNumber); // 松手立即发最终值
    } else {
        pendingServo_.insert(uiNumber, deg);
    }
    emit channelUpdated(0, uiNumber);
    return true;
}

bool ControlViewModel::setVerticalThrusterTarget(int uiNumber, int pct, bool released)
{
    if ((uiNumber < 1) || (uiNumber > wire::kVerticalCount)) {
        return false;
    }
    return setThrusterTarget(uiNumber, pct, released); // 扁平 1..4 = 垂直组
}

bool ControlViewModel::setHorizontalThrusterTarget(int uiNumber, int pct, bool released)
{
    if ((uiNumber < 1) || (uiNumber > wire::kHorizontalCount)) {
        return false;
    }
    return setThrusterTarget(wire::kVerticalCount + uiNumber, pct, released);
}

bool ControlViewModel::setThrusterTarget(int flat, int pct, bool released)
{
    if ((flat < 1) || (flat > thrusters_.size())) {
        return false;
    }
    if (!checkThrusterGroupAllowed(flat <= wire::kVerticalCount)) {
        return false;
    }
    if (pct < thrusterMinPct_) {
        pct = thrusterMinPct_;
    }
    if (pct > thrusterMaxPct_) {
        pct = thrusterMaxPct_;
    }
    thrusters_[flat - 1].target = pct;
    if (released && releaseFlush_) {
        pendingThruster_.remove(flat);
        sendThrusterNow(flat);
    } else {
        pendingThruster_.insert(flat, pct);
    }
    emit channelUpdated(1, flat);
    return true;
}

void ControlViewModel::setVerticalSyncTarget(int pct, bool released)
{
    if (!checkThrusterGroupAllowed(true)) {
        return;
    }
    for (int i = 1; i <= wire::kVerticalCount; ++i) {
        setThrusterTarget(i, pct, released); // 同步滑条：全组同值
    }
}

void ControlViewModel::setHorizontalSyncTarget(int pct, bool released)
{
    if (!checkThrusterGroupAllowed(false)) {
        return;
    }
    for (int i = 1; i <= wire::kHorizontalCount; ++i) {
        setThrusterTarget(wire::kVerticalCount + i, pct, released);
    }
}

bool ControlViewModel::setVerticalBaseTarget(int pct, bool released)
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
    verticalBaseTarget_ = pct;
    if (released && releaseFlush_) {
        pendingVerticalBase_ = false;
        sendBaseNow();
    } else {
        pendingVerticalBase_ = true;
    }
    emit baseUpdated();
    return true;
}

bool ControlViewModel::setHorizontalBaseTarget(int pct, bool released)
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
    horizontalBaseTarget_ = pct;
    if (released && releaseFlush_) {
        pendingHorizontalBase_ = false;
        sendBaseNow();
    } else {
        pendingHorizontalBase_ = true;
    }
    emit baseUpdated();
    return true;
}

bool ControlViewModel::setBaseTarget(int pct, bool released)
{
    // 兼容：旧单基准滑条（Phase 16 拆双基准后移除）
    if (!checkBaseAllowed()) {
        return false;
    }
    if (pct < thrusterMinPct_) {
        pct = thrusterMinPct_;
    }
    if (pct > thrusterMaxPct_) {
        pct = thrusterMaxPct_;
    }
    verticalBaseTarget_ = pct;
    horizontalBaseTarget_ = pct;
    if (released && releaseFlush_) {
        pendingVerticalBase_ = false;
        pendingHorizontalBase_ = false;
        sendBaseNow();
    } else {
        pendingVerticalBase_ = true;
        pendingHorizontalBase_ = true;
    }
    emit baseUpdated();
    return true;
}

void ControlViewModel::servoMid(int uiNumber)
{
    setServoTarget(uiNumber, servoMidDeg_, true);
}

// ---------------------------------------------------------------- Stop/Move

void ControlViewModel::requestMoveAll()
{
    if ((safety_ != nullptr) && !safety_->connected()) {
        emit permissionBlocked(QString::fromLocal8Bit("链路不可用，无法解除推进器停止"));
        return;
    }
    emit sendRequested(static_cast<quint16>(wire::Func::MoveAll), QByteArray());
}

void ControlViewModel::requestStopAll()
{
    if ((safety_ != nullptr) && !safety_->connected()) {
        emit permissionBlocked(QString::fromLocal8Bit("链路不可用，无法停止推进器"));
        return;
    }
    emit sendRequested(static_cast<quint16>(wire::Func::StopAll), QByteArray());
}

void ControlViewModel::requestMoveGroup(bool vertical)
{
    if (safety_ != nullptr) {
        if (!safety_->connected()) {
            emit permissionBlocked(
                    QString::fromLocal8Bit("链路不可用，无法解除分组推进器停止"));
            return;
        }
        // 总使能 OFF/Pending 期间禁止发起分组 Move；重新 ON 后只恢复
        // 未被分组 Stop 锁定的组，不自动恢复或重发旧推进器目标
        if (safety_->switchState(SwitchId::GlobalEnable) != ModeState::On) {
            emit permissionBlocked(
                    QString::fromLocal8Bit("推进器总使能非 ON，禁止发起分组 Move"));
            return;
        }
    }
    emit sendRequested(static_cast<quint16>(vertical ? wire::Func::MoveVertical
                                                     : wire::Func::MoveHorizontal),
                       QByteArray());
}

void ControlViewModel::requestStopGroup(bool vertical)
{
    if ((safety_ != nullptr) && !safety_->connected()) {
        emit permissionBlocked(QString::fromLocal8Bit("链路不可用，无法停止分组推进器"));
        return;
    }
    emit sendRequested(static_cast<quint16>(vertical ? wire::Func::StopVertical
                                                     : wire::Func::StopHorizontal),
                       QByteArray());
}

void ControlViewModel::requestEstop()
{
    if ((safety_ != nullptr)
        && (safety_->estopButton() == EmergencyButtonState::Disabled)) {
        emit permissionBlocked(QString::fromLocal8Bit("链路不可用，紧急停机无法下发"));
        return;
    }
    // 载荷为空：Estop 仅六路推进器置零，绝不携带舵机角度；
    // 绕过合并节拍，TcpClient 紧急队列保证最高优先级
    emit estopRequested(QByteArray());
}

void ControlViewModel::requestEmergency()
{
    if ((safety_ != nullptr)
        && (safety_->emergencyButton() == EmergencyButtonState::Disabled)) {
        emit permissionBlocked(QString::fromLocal8Bit("链路不可用，紧急停机无法下发"));
        return;
    }
    emit emergencyRequested();
}

// ---------------------------------------------------------------- 发送

void ControlViewModel::sendServoNow(int uiNumber)
{
    wire::ServoSetCmd cmd;
    cmd.id = wire::servoWireId(uiNumber); // UI 1..10 -> wire 0..9
    cmd.angleDeg = static_cast<quint16>(servos_.at(uiNumber - 1).target);
    const QByteArray payload = wire::encodeServoSet(cmd);
    if (payload.isEmpty()) {
        return; // 值域/ID 校验失败（编码器拒绝）
    }
    servos_[uiNumber - 1].sent = servos_.at(uiNumber - 1).target;
    emit sendRequested(static_cast<quint16>(wire::Func::ServoSet), payload);
}

void ControlViewModel::sendThrusterNow(int flat)
{
    wire::PropellerSetCmd cmd;
    // 扁平 1..4=垂直 5..6=水平 -> wire 10..15
    cmd.id = wire::thrusterWireIdFromFlat(flat);
    cmd.valuePct = static_cast<qint16>(thrusters_.at(flat - 1).target);
    const QByteArray payload = wire::encodePropellerSet(cmd);
    if (payload.isEmpty()) {
        return;
    }
    thrusters_[flat - 1].sent = thrusters_.at(flat - 1).target;
    emit sendRequested(static_cast<quint16>(wire::Func::PropellerSet), payload);
}

void ControlViewModel::sendBaseNow()
{
    // 姿态稳定基准 BaseValueVH：单帧 2×i16（垂直基准、水平基准）
    const QByteArray payload = wire::encodeBaseValueVH(
            static_cast<qint16>(verticalBaseTarget_),
            static_cast<qint16>(horizontalBaseTarget_));
    if (payload.isEmpty()) {
        return;
    }
    for (int i = 0; i < thrusters_.size(); ++i) {
        thrusters_[i].sent = (i < wire::kVerticalCount) ? verticalBaseTarget_
                                                        : horizontalBaseTarget_;
    }
    emit sendRequested(static_cast<quint16>(wire::Func::BaseValueVH), payload);
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
        const int flat = it.key();
        // 组停止锁存/使能请求中：丢弃该组待发（重新使能不重放红线）
        if ((safety_ != nullptr)
            && !safety_->canThrusterGroup(flat <= wire::kVerticalCount)) {
            continue;
        }
        sendThrusterNow(flat);
    }
    if (pendingVerticalBase_ || pendingHorizontalBase_) {
        pendingVerticalBase_ = false;
        pendingHorizontalBase_ = false;
        if ((safety_ == nullptr) || safety_->canBaseSlider()) {
            sendBaseNow();
        }
    }
}

// ---------------------------------------------------------------- ACK/超时回填

void ControlViewModel::onFrameSent(quint16 seq, quint16 funcId,
                                   const QByteArray& payload)
{
    SentTrack track;
    if (funcId == static_cast<quint16>(wire::Func::ServoSet)) {
        track.kind = 0;
        // payload 首字节为 wire id（0..9），转 UI 编号回填
        track.id = payload.isEmpty()
                ? 0 : wire::servoUiNumber(static_cast<quint8>(payload.at(0)));
        track.value = servos_.value(track.id - 1).sent;
    } else if (funcId == static_cast<quint16>(wire::Func::PropellerSet)) {
        track.kind = 1;
        // payload 首字节为 wire id（10..15），转扁平序号回填
        track.id = payload.isEmpty()
                ? 0 : wire::thrusterFlatFromWireId(static_cast<quint8>(payload.at(0)));
        track.value = thrusters_.value(track.id - 1).sent;
    } else if (funcId == static_cast<quint16>(wire::Func::BaseValueVH)) {
        track.kind = 2;
        // 双基准值直接从载荷回读（以实际发送值为准）
        bool ok = false;
        track.valueV = (payload.size() >= 4) ? wire::getI16(payload, 0, ok)
                                             : verticalBaseTarget_;
        track.valueH = (payload.size() >= 4) ? wire::getI16(payload, 2, ok)
                                             : horizontalBaseTarget_;
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
            // 双组分别确认到各自基准值
            for (int i = 0; i < thrusters_.size(); ++i) {
                thrusters_[i].confirmed =
                        (i < wire::kVerticalCount) ? track.valueV : track.valueH;
                thrusters_[i].confirmedValid = true;
            }
        } else {
            for (ChannelVm& ch : thrusters_) {
                ch.target = ch.confirmed; // 被拒：回滚最近确认值
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
        for (int flat = 1; flat <= thrusters_.size(); ++flat) {
            emit channelUnknown(1, flat);
        }
    }
}

// ---------------------------------------------------------------- 权威状态同步

void ControlViewModel::onAuthorityStateChanged()
{
    if (safety_ == nullptr) {
        return;
    }
    // 姿态稳定 ON：切基准模式，清空全部逐路待发
    //（隐藏控件不得稍后发送旧值红线）；退出基准模式无恢复动作，由 UI 重新驱动
    if (safety_->switchState(SwitchId::AttitudeStab) == ModeState::On) {
        pendingThruster_.clear();
    }
    // 分组停止锁存/使能请求中：清空该组逐路待发（重新使能不重放红线）
    if (!safety_->canThrusterGroup(true)) {
        clearGroupPending(true);
    }
    if (!safety_->canThrusterGroup(false)) {
        clearGroupPending(false);
    }
    // 基准不可用（稳定 OFF/组锁存/请求中）：清基准待发
    if (!safety_->canBaseSlider()) {
        pendingVerticalBase_ = false;
        pendingHorizontalBase_ = false;
    }
}

void ControlViewModel::clearGroupPending(bool vertical)
{
    const int first = vertical ? 1 : wire::kVerticalCount + 1;
    const int last = vertical ? wire::kVerticalCount
                              : static_cast<int>(thrusters_.size());
    for (int flat = first; flat <= last; ++flat) {
        pendingThruster_.remove(flat);
    }
}

} // namespace salacia
