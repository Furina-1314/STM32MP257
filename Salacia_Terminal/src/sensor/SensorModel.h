#pragma once

#include <QObject>
#include <QMetaType>

#include <QPair>
#include <QVector>

#include "communication/FunctionRegistry.h"
#include "core/DataManager.h"
#include "sensor/MPU6500Processor.h"

namespace salacia {

// DYP-RD 前向测距状态分级（不用 0 冒充无效值红线的显示侧落点）
enum class DypState
{
    NotReady,    // 有效位未置/哨兵值
    Normal,      // 正常量程内
    Warning,     // 距离 < warn_distance（过近警示）
    Danger,      // 距离 < danger_distance（碰撞危险）
    OutOfRange,  // 超出 [valid_min, valid_max]
    Stale,       // 数据过期
};

// 合并后的传感器显示状态（TCP 100Hz 与 UDP 遥测双源取新鲜度优者）
struct SensorDisplay
{
    float tempC = 0.0F;
    bool tempValid = false;
    float humidPct = 0.0F;
    bool humidValid = false;
    float voltage = 0.0F;
    bool voltageValid = false;
    float socPct = 0.0F;
    bool socCalibrated = false; // SOC 曲线未标定 -> UI 显示"待标定"
    bool batteryLow = false;
    bool batteryCritical = false;
    float distMm = 0.0F;
    DypState dypState = DypState::NotReady;
    qint64 lastUpdateMs = 0;
    enum class Source
    {
        None,
        Tcp,
        Udp
    };
    Source source = Source::None;
};

// 传感器数据模型（主线程；TCP/UDP 双源 -> 新鲜度取优 -> 分级/折算）
//
// 输入：
//  - applyTcpSummary：TcpClient::sensorSummaryReady（工作线程发、此处排队收）
//  - applyUdpState：  现有 UDP 遥测路径的 RovState（MainWindow 桥接）
// 输出：
//  - current()：合并后的显示状态（调用时计算新鲜度，无内部定时器）
//  - attitudeReady：TCP 姿态经 Mahony 解算且较 UDP 新时发出（供 DataManager）
//  - batteryAlarm/dypStateChanged：越限/分级变化（供告警中心）
//
// SOC：仅按 [battery] soc_curve 分段线性折算；曲线缺失 -> socCalibrated=false
//（显示"待标定"，保留原始电压）；输出钳位 0-100，一阶滤波 + 滞回。
class SensorModel : public QObject
{
    Q_OBJECT

public:
    explicit SensorModel(QObject* parent = nullptr);

    void applyTcpSummary(const wire::SensorSummary& summary);
    void applyUdpState(const RovState& state);

    SensorDisplay current() const;

    // 测试钩子：清空双源状态
    void resetForTest();

signals:
    void displayUpdated();
    void attitudeReady(const RovState& state);
    void batteryAlarm(bool low, bool critical);
    void dypStateChanged(salacia::DypState state);

private:
    float computeSoc(float voltage) const;
    DypState gradeDistance(float distMm) const;
    void refreshSoc(float voltage); // 折算+滤波+滞回+越限告警

    // ---- 配置快照（[battery]/[dyp]/[tcp]，构造时读取）----
    QVector<QPair<float, float>> socCurve_; // (voltage, socPct) 单调
    bool socCalibrated_ = false;
    float socFilterAlpha_ = 0.2F;
    float socHysteresisPct_ = 3.0F;
    float batteryLowPct_ = 30.0F;
    float batteryCriticalPct_ = 15.0F;
    float dypValidMinMm_ = 20.0F;
    float dypValidMaxMm_ = 8000.0F;
    float dypWarnMm_ = 1000.0F;
    float dypDangerMm_ = 300.0F;
    int sensorStaleMs_ = 500;
    int dypStaleMs_ = 500;

    // ---- TCP 源缓存 ----
    qint64 lastTcpMs_ = 0;
    float tcpTempC_ = 0.0F;
    float tcpHumidPct_ = 0.0F;
    float tcpVoltage_ = 0.0F;
    float tcpDistMm_ = -1.0F;
    quint8 tcpValidMask_ = 0U;

    // ---- UDP 源缓存 ----
    qint64 lastUdpMs_ = 0;
    float udpTempC_ = 0.0F;
    float udpHumidPct_ = 0.0F;
    float udpVoltage_ = 0.0F;

    // ---- SOC 滤波/滞回状态 ----
    mutable float socFilteredPct_ = -1.0F; // <0 = 未初始化
    float socPctCached_ = 0.0F;
    bool batteryLowActive_ = false;
    bool batteryCriticalActive_ = false;

    // ---- DYP 分级状态 ----
    DypState dypState_ = DypState::NotReady;
    qint64 lastDypMs_ = 0;

    MPU6500Processor mpu_; // TCP 姿态解算（与 UDP 路径独立实例）
};

} // namespace salacia

Q_DECLARE_METATYPE(salacia::DypState)
