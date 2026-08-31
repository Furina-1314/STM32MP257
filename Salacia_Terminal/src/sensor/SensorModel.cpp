#include "SensorModel.h"

#include <QDateTime>

#include "core/AppConfig.h"

namespace salacia {

namespace {

// 解析 [battery] soc_curve："v1 v2 ... : s1 s2 ..."（伏:百分比，分段线性；
// 空格分隔——逗号在 QSettings IniFormat 下会退化为列表，见 AppConfig 读取侧）
bool parseSocCurve(const QString& text, QVector<QPair<float, float>>& out)
{
    const int sep = text.indexOf(QLatin1Char(':'));
    if (sep <= 0) {
        return false;
    }
    const QStringList volts = text.left(sep).split(QLatin1Char(' '),
                                                   Qt::SkipEmptyParts);
    const QStringList socs = text.mid(sep + 1).split(QLatin1Char(' '),
                                                     Qt::SkipEmptyParts);
    if (volts.size() != socs.size() || volts.size() < 2) {
        return false;
    }
    QVector<QPair<float, float>> points;
    for (int i = 0; i < volts.size(); ++i) {
        bool okV = false;
        bool okS = false;
        const float v = volts.at(i).toFloat(&okV);
        const float s = socs.at(i).toFloat(&okS);
        if (!okV || !okS || (s < 0.0F) || (s > 100.0F)) {
            return false;
        }
        points.append(qMakePair(v, s));
    }
    for (int i = 1; i < points.size(); ++i) {
        if (points.at(i).first <= points.at(i - 1).first) {
            return false; // 电压轴必须严格单调递增（曲线单调红线）
        }
    }
    out = points;
    return true;
}

} // namespace

SensorModel::SensorModel(QObject* parent)
    : QObject(parent)
{
    const AppConfig& cfg = AppConfig::instance();
    socCalibrated_ = parseSocCurve(cfg.socCurve(), socCurve_);
    socFilterAlpha_ = cfg.socFilterAlpha();
    socHysteresisPct_ = cfg.socHysteresisPct();
    batteryLowPct_ = cfg.batteryLowThresholdPct();
    batteryCriticalPct_ = cfg.batteryCriticalThresholdPct();
    dypValidMinMm_ = cfg.dypValidMin();
    dypValidMaxMm_ = cfg.dypValidMax();
    dypWarnMm_ = cfg.dypWarnDistance();
    dypDangerMm_ = cfg.dypDangerDistance();
    sensorStaleMs_ = cfg.sensorStaleMs();
    dypStaleMs_ = cfg.dypStaleMs();
}

void SensorModel::resetForTest()
{
    lastTcpMs_ = 0;
    lastUdpMs_ = 0;
    lastDypMs_ = 0;
    socFilteredPct_ = -1.0F;
    dypState_ = DypState::NotReady;
    batteryLowActive_ = false;
    batteryCriticalActive_ = false;
    tcpValidMask_ = 0U;
}

void SensorModel::applyTcpSummary(const wire::SensorSummary& summary)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    lastTcpMs_ = now;
    tcpTempC_ = summary.tempC;
    tcpHumidPct_ = summary.humidPct;
    tcpVoltage_ = summary.voltage;
    tcpValidMask_ = summary.validMask;

    // DYP-RD 分级（有效位 + 量程 + 阈值；哨兵值视为未就绪）
    if ((summary.validMask & wire::kValidDyp) != 0U) {
        tcpDistMm_ = summary.distMm;
        lastDypMs_ = now;
        const DypState graded = gradeDistance(summary.distMm);
        if (graded != dypState_) {
            dypState_ = graded;
            emit dypStateChanged(graded);
        }
    } else {
        tcpDistMm_ = -1.0F;
        if (dypState_ != DypState::NotReady) {
            dypState_ = DypState::NotReady;
            emit dypStateChanged(DypState::NotReady);
        }
    }

    // 姿态：TCP 六轴走独立 Mahony 实例；结果较 UDP 新时发布
    if ((summary.validMask & wire::kValidMpu) != 0U) {
        RawImuSample sample;
        sample.hostTimeMs = static_cast<quint64>(now);
        sample.boardTimeMs = summary.boardTimeMs;
        sample.sequence = static_cast<quint16>(summary.boardTimeMs & 0xFFFFU);
        for (int i = 0; i < 3; ++i) {
            sample.accelMps2[i] = summary.accelMps2[i];
            sample.gyroRadS[i] = summary.gyroRadS[i];
        }
        const RovState attitude = mpu_.process(sample);
        RovState merged = attitude;
        merged.cabinTempC = summary.tempC;
        merged.cabinHumidityPct = summary.humidPct;
        merged.batteryVoltage = summary.voltage;
        merged.timestampMs = now;
        emit attitudeReady(merged);
    }

    if ((summary.validMask & wire::kValidVoltage) != 0U) {
        refreshSoc(summary.voltage);
    }
    emit displayUpdated();
}

void SensorModel::applyUdpState(const RovState& state)
{
    lastUdpMs_ = QDateTime::currentMSecsSinceEpoch();
    udpTempC_ = state.cabinTempC;
    udpHumidPct_ = state.cabinHumidityPct;
    udpVoltage_ = state.batteryVoltage;
    refreshSoc(state.batteryVoltage);
    emit displayUpdated();
}

void SensorModel::refreshSoc(float voltage)
{
    socPctCached_ = computeSoc(voltage);
    const bool low = (socPctCached_ <= batteryLowPct_);
    const bool critical = (socPctCached_ <= batteryCriticalPct_);
    if ((low != batteryLowActive_) || (critical != batteryCriticalActive_)) {
        batteryLowActive_ = low;
        batteryCriticalActive_ = critical;
        emit batteryAlarm(low, critical);
    }
}

float SensorModel::computeSoc(float voltage) const
{
    if (!socCalibrated_) {
        return 0.0F; // 未标定：socCalibrated=false，调用方显示"待标定"
    }
    // 分段线性插值（曲线外按端点钳位）
    float soc = 0.0F;
    const auto& curve = socCurve_;
    if (voltage <= curve.first().first) {
        soc = curve.first().second;
    } else if (voltage >= curve.last().first) {
        soc = curve.last().second;
    } else {
        for (int i = 1; i < curve.size(); ++i) {
            if (voltage <= curve.at(i).first) {
                const float v0 = curve.at(i - 1).first;
                const float s0 = curve.at(i - 1).second;
                const float v1 = curve.at(i).first;
                const float s1 = curve.at(i).second;
                const float t = (voltage - v0) / (v1 - v0);
                soc = s0 + t * (s1 - s0);
                break;
            }
        }
    }
    if (soc < 0.0F) {
        soc = 0.0F;
    }
    if (soc > 100.0F) {
        soc = 100.0F;
    }
    // 一阶滤波 + 滞回（异常跳变按配置抑制）
    if (socFilteredPct_ < 0.0F) {
        socFilteredPct_ = soc;
    } else {
        const float blended = socFilteredPct_
                + socFilterAlpha_ * (soc - socFilteredPct_);
        if (qAbs(blended - socFilteredPct_) >= socHysteresisPct_) {
            socFilteredPct_ = blended;
        }
    }
    return socFilteredPct_;
}

DypState SensorModel::gradeDistance(float distMm) const
{
    if ((distMm < dypValidMinMm_) || (distMm > dypValidMaxMm_)) {
        return DypState::OutOfRange;
    }
    if (distMm <= dypDangerMm_) {
        return DypState::Danger;
    }
    if (distMm <= dypWarnMm_) {
        return DypState::Warning;
    }
    return DypState::Normal;
}

SensorDisplay SensorModel::current() const
{
    SensorDisplay d;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const bool tcpFresh = (lastTcpMs_ > 0) && ((now - lastTcpMs_) <= sensorStaleMs_);
    const bool udpFresh = (lastUdpMs_ > 0) && ((now - lastUdpMs_) <= sensorStaleMs_);
    const bool tcpNewer = lastTcpMs_ >= lastUdpMs_;

    // 双源取优：均新鲜/均过期时取更新的一方；单侧新鲜直接选它
    const bool useTcp = tcpFresh && (!udpFresh || tcpNewer);
    const bool useUdp = udpFresh && (!tcpFresh || !tcpNewer);

    if (useTcp) {
        d.source = SensorDisplay::Source::Tcp;
        d.lastUpdateMs = lastTcpMs_;
        d.tempValid = (tcpValidMask_ & wire::kValidTempHum) != 0U;
        d.tempC = tcpTempC_;
        d.humidValid = d.tempValid;
        d.humidPct = tcpHumidPct_;
        d.voltageValid = (tcpValidMask_ & wire::kValidVoltage) != 0U;
        d.voltage = tcpVoltage_;
    } else if (useUdp) {
        d.source = SensorDisplay::Source::Udp;
        d.lastUpdateMs = lastUdpMs_;
        // UDP v2 无有效位掩码：链路新鲜即视为有效（协议固化语义）
        d.tempValid = true;
        d.tempC = udpTempC_;
        d.humidValid = true;
        d.humidPct = udpHumidPct_;
        d.voltageValid = true;
        d.voltage = udpVoltage_;
    } else {
        d.source = SensorDisplay::Source::None;
        d.dypState = DypState::Stale;
        return d; // 双源均过期：全部无效（不用 0 冒充）
    }

    d.socCalibrated = socCalibrated_;
    if (d.voltageValid) {
        d.socPct = socPctCached_;
        d.batteryLow = batteryLowActive_;
        d.batteryCritical = batteryCriticalActive_;
    }

    // DYP：仅 TCP 源提供；过期 -> Stale
    if (useTcp && ((tcpValidMask_ & wire::kValidDyp) != 0U)) {
        if ((lastDypMs_ > 0) && ((now - lastDypMs_) > dypStaleMs_)) {
            d.dypState = DypState::Stale;
        } else {
            d.dypState = dypState_;
            d.distMm = tcpDistMm_;
        }
    } else if (useUdp) {
        d.dypState = DypState::NotReady; // UDP v2 无测距字段
    }
    return d;
}

} // namespace salacia
