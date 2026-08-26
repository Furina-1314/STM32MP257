#pragma once

#include <QObject>

#include "core/DataManager.h"

namespace salacia {

// 三维姿态视图的 QML 适配模型（单 NOTIFY，20Hz 通知节流由消费侧自理）
//
// 数据源：DataManager（遥测线程写入）经排队信号在主线程刷新缓存，
// QML 属性绑定读取（线程红线：QML 绝不直接触碰 DataManager）。
class RovVizModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(float qw READ qw NOTIFY stateChanged)
    Q_PROPERTY(float qx READ qx NOTIFY stateChanged)
    Q_PROPERTY(float qy READ qy NOTIFY stateChanged)
    Q_PROPERTY(float qz READ qz NOTIFY stateChanged)
    Q_PROPERTY(float rollDeg READ rollDeg NOTIFY stateChanged)
    Q_PROPERTY(float pitchDeg READ pitchDeg NOTIFY stateChanged)
    Q_PROPERTY(float yawDeg READ yawDeg NOTIFY stateChanged)
    Q_PROPERTY(float cabinTempC READ cabinTempC NOTIFY stateChanged)
    Q_PROPERTY(float cabinHumidityPct READ cabinHumidityPct NOTIFY stateChanged)
    Q_PROPERTY(float batteryVoltage READ batteryVoltage NOTIFY stateChanged)
    Q_PROPERTY(float batteryPercent READ batteryPercent NOTIFY stateChanged)
    Q_PROPERTY(bool telemetryActive READ telemetryActive NOTIFY stateChanged)

public:
    explicit RovVizModel(QObject* parent = nullptr);

    // 连接 DataManager 队列信号（主线程调用一次）
    void bindToDataManager();

    float qw() const { return cached_.quaternion[0]; }
    float qx() const { return cached_.quaternion[1]; }
    float qy() const { return cached_.quaternion[2]; }
    float qz() const { return cached_.quaternion[3]; }
    float rollDeg() const { return cached_.rollDeg; }
    float pitchDeg() const { return cached_.pitchDeg; }
    float yawDeg() const { return cached_.yawDeg; }
    float cabinTempC() const { return cached_.cabinTempC; }
    float cabinHumidityPct() const { return cached_.cabinHumidityPct; }
    float batteryVoltage() const { return cached_.batteryVoltage; }
    float batteryPercent() const { return cached_.batteryPercent; }
    bool telemetryActive() const { return active_; }

signals:
    void stateChanged(); // 全属性统一通知（20Hz）

private:
    void refresh();

    RovState cached_;
    bool active_ = false;
};

} // namespace salacia
