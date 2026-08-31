// SensorModel 单元测试：双源取优 / SOC 曲线 / DYP 分级 / 有效性
#include <QtTest/qtest.h>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "core/AppConfig.h"
#include "sensor/SensorModel.h"

using namespace salacia;

namespace {

bool loadIni(const QString& extra)
{
    static QTemporaryDir dir;
    const QString base = QStringLiteral(R"INI(
[tcp]
sensor_stale_ms = 400

[battery]
cell_count = 4
chemistry = liion
soc_curve = 13.0,14.0,15.0,16.8:0,20,80,100
soc_filter_alpha = 1.0
soc_hysteresis_pct = 0
low_threshold_pct = 30
critical_threshold_pct = 15

[dyp]
valid_min_mm = 20
valid_max_mm = 8000
warn_distance_mm = 1000
danger_distance_mm = 300
stale_ms = 400
)INI");
    const QString path = dir.filePath(QStringLiteral("app_config.ini"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write((base + extra).toUtf8());
    file.close();
    return AppConfig::instance().load(path);
}

wire::SensorSummary makeSummary(float temp, float hum, float volt, float dist,
                                quint8 mask)
{
    wire::SensorSummary s;
    s.tempC = temp;
    s.humidPct = hum;
    s.voltage = volt;
    s.distMm = dist;
    s.validMask = mask;
    for (int i = 0; i < 3; ++i) {
        s.accelMps2[i] = 0.0F;
        s.gyroRadS[i] = 0.0F;
    }
    s.accelMps2[2] = 9.8F;
    s.boardTimeMs = 100U;
    return s;
}

constexpr quint8 kAllValid = wire::kValidTempHum | wire::kValidMpu
        | wire::kValidVoltage | wire::kValidDyp;

} // namespace

class TestSensorModel : public QObject
{
    Q_OBJECT

private slots:
    void socCurveInterpolation();
    void socUncalibrated();
    void socClampAndHysteresis();
    void dypGrading();
    void freshnessMerge();
    void staleWindow();
    void invalidNotZero();
    void attitudeFromTcp();

private:
    void reload() { QVERIFY(loadIni(QString())); }
};

void TestSensorModel::socCurveInterpolation()
{
    reload();
    SensorModel model;
    model.applyTcpSummary(makeSummary(26.0F, 50.0F, 15.2F, 500.0F, kAllValid));
    const SensorDisplay d = model.current();
    QVERIFY2(d.socCalibrated,
             qPrintable(QStringLiteral("curve=[%1] cell=%2 loaded=%3")
                            .arg(AppConfig::instance().socCurve())
                            .arg(AppConfig::instance().cellCount())
                            .arg(AppConfig::instance().loaded())));
    // 15.2V 落在 (15.0,80)-(16.8,100) 段：80 + 0.2/1.8*20 = 82.2
    QVERIFY(qAbs(d.socPct - 82.2F) < 0.5F);
    QCOMPARE(d.tempC, 26.0F);
    QCOMPARE(d.humidPct, 50.0F);
    QCOMPARE(d.source, SensorDisplay::Source::Tcp);
}

void TestSensorModel::socUncalibrated()
{
    // 空 soc_curve -> 待标定（socCalibrated=false，电压仍保留）
    static QTemporaryDir dir2;
    const QString path = dir2.filePath(QStringLiteral("no_curve.ini"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(QByteArrayLiteral(
            "[battery]\ncell_count = 4\nsoc_curve = \n"));
    file.close();
    QVERIFY(AppConfig::instance().load(path));

    SensorModel model;
    model.applyTcpSummary(makeSummary(26.0F, 50.0F, 15.2F, 500.0F, kAllValid));
    const SensorDisplay d = model.current();
    QVERIFY(!d.socCalibrated); // 显示"待标定"
    QVERIFY(d.voltageValid);
    QCOMPARE(d.voltage, 15.2F); // 原始电压保留
}

void TestSensorModel::socClampAndHysteresis()
{
    reload();
    SensorModel model;
    // 低于曲线下限 -> 0%；高于上限 -> 100%
    model.applyTcpSummary(makeSummary(26, 50, 10.0F, 500, kAllValid));
    QCOMPARE(model.current().socPct, 0.0F);
    model.applyTcpSummary(makeSummary(26, 50, 17.5F, 500, kAllValid));
    QCOMPARE(model.current().socPct, 100.0F);

    // 滞回：本用例 ini 配置 hysteresis=0 -> 不启用；大步进正常更新
    model.applyTcpSummary(makeSummary(26, 50, 14.0F, 500, kAllValid));
    QVERIFY(model.current().socPct < 25.0F);
}

void TestSensorModel::dypGrading()
{
    reload();
    SensorModel model;
    // 未就绪（有效位未置）
    model.applyTcpSummary(makeSummary(26, 50, 15.0F, 300.0F,
                                      kAllValid & ~wire::kValidDyp));
    QCOMPARE(model.current().dypState, DypState::NotReady);

    model.applyTcpSummary(makeSummary(26, 50, 15.0F, 100.0F, kAllValid));
    QCOMPARE(model.current().dypState, DypState::Danger);   // < 300

    model.applyTcpSummary(makeSummary(26, 50, 15.0F, 500.0F, kAllValid));
    QCOMPARE(model.current().dypState, DypState::Warning);  // < 1000

    model.applyTcpSummary(makeSummary(26, 50, 15.0F, 2000.0F, kAllValid));
    QCOMPARE(model.current().dypState, DypState::Normal);

    model.applyTcpSummary(makeSummary(26, 50, 15.0F, 9000.0F, kAllValid));
    QCOMPARE(model.current().dypState, DypState::OutOfRange);

    // 哨兵值（有效位误置）-> 量程外
    model.applyTcpSummary(makeSummary(26, 50, 15.0F, -1.0F, kAllValid));
    QCOMPARE(model.current().dypState, DypState::OutOfRange);
}

void TestSensorModel::freshnessMerge()
{
    reload();
    SensorModel model;
    // TCP 先到
    model.applyTcpSummary(makeSummary(20.0F, 40.0F, 15.0F, 500.0F, kAllValid));
    QCOMPARE(model.current().source, SensorDisplay::Source::Tcp);
    // UDP 更新 -> 取 UDP（且无 DYP 字段 -> NotReady）
    RovState udp;
    udp.cabinTempC = 25.0F;
    udp.cabinHumidityPct = 60.0F;
    udp.batteryVoltage = 14.0F;
    QTest::qWait(3); // 确保时间戳严格更新
    model.applyUdpState(udp);
    SensorDisplay d = model.current();
    QCOMPARE(d.source, SensorDisplay::Source::Udp);
    QCOMPARE(d.tempC, 25.0F);
    QCOMPARE(d.dypState, DypState::NotReady);
    // TCP 再更新 -> 回到 TCP
    QTest::qWait(3);
    model.applyTcpSummary(makeSummary(21.0F, 41.0F, 15.1F, 600.0F, kAllValid));
    d = model.current();
    QCOMPARE(d.source, SensorDisplay::Source::Tcp);
    QCOMPARE(d.tempC, 21.0F);
}

void TestSensorModel::staleWindow()
{
    reload(); // sensor_stale_ms = 400
    SensorModel model;
    model.applyTcpSummary(makeSummary(26, 50, 15.0F, 500, kAllValid));
    QVERIFY(model.current().tempValid);
    QTest::qWait(600);
    const SensorDisplay d = model.current();
    QCOMPARE(d.source, SensorDisplay::Source::None); // 双源过期
    QVERIFY(!d.tempValid);   // 不用 0 冒充：有效性显式失效
    QVERIFY(!d.voltageValid);
    QCOMPARE(d.dypState, DypState::Stale);
}

void TestSensorModel::invalidNotZero()
{
    reload();
    SensorModel model;
    // 温湿度有效位未置：数值无效（valid=false），不显示 0 冒充
    model.applyTcpSummary(makeSummary(0.0F, 0.0F, 15.0F, 500.0F,
                                      kAllValid & ~wire::kValidTempHum));
    const SensorDisplay d = model.current();
    QVERIFY(!d.tempValid);
    QVERIFY(!d.humidValid);
    QVERIFY(d.voltageValid); // 其余字段不受影响
}

void TestSensorModel::attitudeFromTcp()
{
    reload();
    SensorModel model;
    QSignalSpy spy(&model, &SensorModel::attitudeReady);
    model.applyTcpSummary(makeSummary(26, 50, 15.0F, 500, kAllValid));
    QCOMPARE(spy.count(), 1);
    const RovState state = spy.first().at(0).value<RovState>();
    QVERIFY(qAbs(state.quaternion[0]) > 0.9F); // 静止初始姿态接近单位四元数
    QCOMPARE(state.cabinTempC, 26.0F);

    // MPU 有效位未置 -> 不产生姿态
    QSignalSpy spy2(&model, &SensorModel::attitudeReady);
    model.applyTcpSummary(makeSummary(26, 50, 15.0F, 500,
                                      kAllValid & ~wire::kValidMpu));
    QCOMPARE(spy2.count(), 0);
}

QTEST_MAIN(TestSensorModel) // qWait/QSignalSpy 需事件循环
#include "test_sensormodel.moc"
