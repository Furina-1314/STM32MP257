// AppConfig 单元测试：新节键读取、越界回退、交叉校验降级、旧键兼容
#include <QtTest/qtest.h>  // 窄化包含：避开 QtCore 伞头（qthreadpool->qrunnable 的 const 默认构造与 /permissive- 冲突）

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "core/AppConfig.h"

using namespace salacia;

namespace {

// 在临时目录生成 ini 并显式加载（AppConfig 为单例，逐用例重载）
bool loadFromText(QTemporaryDir& dir, const QString& iniText)
{
    const QString path = dir.filePath(QStringLiteral("app_config.ini"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(iniText.toUtf8());
    file.close();
    return AppConfig::instance().load(path);
}

const QString kGoodIni = QStringLiteral(R"INI(
[network]
host_ip =
board_ip = 192.168.1.120
rtp_port = 5000
telemetry_port = 5001
telemetry_udp_enable = true
telemetry_watchdog_ms = 500
telemetry_stale_ms = 1000

[tcp]
enable = true
host =
port = 7000
connect_timeout_ms = 3000
request_timeout_ms = 1000
heartbeat_enable = true
heartbeat_interval_ms = 1000
reconnect_enable = true
reconnect_base_ms = 1000
reconnect_max_ms = 10000
max_retry = 0
tcp_nodelay = true
recv_buffer_limit = 65536
max_payload = 4096
send_queue_capacity = 64
sensor_expected_hz = 100
sensor_stale_ms = 500

[control]
servo_count = 10
thruster_count = 6
id_base = 1
servo_min_deg = 0
servo_max_deg = 180
servo_step_deg = 1
thruster_min_pct = -100
thruster_max_pct = 100
thruster_step_pct = 1
slider_rate_limit_ms = 50

[battery]
cell_count = 4
chemistry =
soc_curve =
soc_filter_alpha = 0.2
low_threshold_pct = 30
critical_threshold_pct = 15

[dyp]
unit = mm
precision = 0
valid_min_mm = 20
valid_max_mm = 8000
stale_ms = 500
warn_distance_mm = 1000
danger_distance_mm = 300

[alarms]
max_items = 200
merge_window_ms = 5000

[ui]
theme = light
palette = fluent
text_refresh_hz = 5
angle_precision = 1
voltage_precision = 2
estop_confirm = false
emergency_confirm = true
)INI");

} // namespace

class TestAppConfig : public QObject
{
    Q_OBJECT

private slots:
    void newKeysLoad()
    {
        QTemporaryDir dir;
        QVERIFY(loadFromText(dir, kGoodIni));
        const AppConfig& cfg = AppConfig::instance();

        QCOMPARE(cfg.tcpUsable(), true);
        QCOMPARE(cfg.tcpPort(), quint16(7000));
        QCOMPARE(cfg.tcpHost(), QStringLiteral("192.168.1.120")); // 空 host 回退 board_ip
        QCOMPARE(cfg.sensorExpectedHz(), 100);
        QCOMPARE(cfg.sensorStaleMs(), 500);
        QCOMPARE(cfg.servoCount(), 10);
        QCOMPARE(cfg.thrusterCount(), 6);
        QCOMPARE(cfg.sliderRateLimitMs(), 50);
        QCOMPARE(cfg.cellCount(), 4);
        QCOMPARE(cfg.dypUnit(), QStringLiteral("mm"));
        QCOMPARE(cfg.dypWarnDistance(), 1000.0F);
        QCOMPARE(cfg.alarmMaxItems(), 200);
        QCOMPARE(cfg.uiTheme(), QStringLiteral("light"));
        QCOMPARE(cfg.uiPalette(), QStringLiteral("fluent"));
        QCOMPARE(cfg.estopConfirmEnabled(), false);
        QCOMPARE(cfg.emergencyConfirmEnabled(), true);
        QCOMPARE(cfg.telemetryUdpEnabled(), true);
        QVERIFY(cfg.validationIssues().isEmpty());
    }

    void invalidValuesFallBack()
    {
        QString ini = kGoodIni;
        ini.replace(QStringLiteral("port = 7000"), QStringLiteral("port = 0"));
        ini.replace(QStringLiteral("servo_count = 10"), QStringLiteral("servo_count = 99"));
        QTemporaryDir dir;
        QVERIFY(loadFromText(dir, ini));
        const AppConfig& cfg = AppConfig::instance();

        QCOMPARE(cfg.tcpPort(), quint16(7000)); // 越界回退内置默认
        QCOMPARE(cfg.servoCount(), 10);
    }

    void crossValidationDisablesTcp()
    {
        QString ini = kGoodIni;
        ini.replace(QStringLiteral("reconnect_max_ms = 10000"),
                    QStringLiteral("reconnect_max_ms = 500"));
        QTemporaryDir dir;
        QVERIFY(loadFromText(dir, ini));
        const AppConfig& cfg = AppConfig::instance();

        QVERIFY(!cfg.validationIssues().isEmpty());
        QCOMPARE(cfg.tcpUsable(), false); // 冲突值 -> TCP 禁用降级
    }

    void dypConstraintChecked()
    {
        QString ini = kGoodIni;
        ini.replace(QStringLiteral("warn_distance_mm = 1000"),
                    QStringLiteral("warn_distance_mm = 100")); // warn <= danger
        QTemporaryDir dir;
        QVERIFY(loadFromText(dir, ini));
        QVERIFY(!AppConfig::instance().validationIssues().isEmpty());
    }

    void themeEnumChecked()
    {
        QString ini = kGoodIni;
        ini.replace(QStringLiteral("theme = light"), QStringLiteral("theme = purple"));
        QTemporaryDir dir;
        QVERIFY(loadFromText(dir, ini));
        QVERIFY(!AppConfig::instance().validationIssues().isEmpty());
    }

    void tcpDisabledBySwitch()
    {
        QString ini = kGoodIni;
        ini.replace(QStringLiteral("enable = true"), QStringLiteral("enable = false"));
        QTemporaryDir dir;
        QVERIFY(loadFromText(dir, ini));
        QCOMPARE(AppConfig::instance().tcpUsable(), false);
    }

    void tcpHostOverride()
    {
        QString ini = kGoodIni;
        ini.replace(QStringLiteral("host =\n"), QStringLiteral("host = 10.0.0.9\n"));
        QTemporaryDir dir;
        QVERIFY(loadFromText(dir, ini));
        QCOMPARE(AppConfig::instance().tcpHost(), QStringLiteral("10.0.0.9"));
    }

    void legacyKeysStillLoad()
    {
        // 旧键兼容：[network]/[video]/[ai] 现有键在新配置下不受影响
        QTemporaryDir dir;
        QVERIFY(loadFromText(dir, kGoodIni));
        const AppConfig& cfg = AppConfig::instance();
        QCOMPARE(cfg.videoRtpPort(), quint16(5000));
        QCOMPARE(cfg.telemetryPort(), quint16(5001));
        QCOMPARE(cfg.boardIp(), QStringLiteral("192.168.1.120"));
    }
};

QTEST_APPLESS_MAIN(TestAppConfig)
#include "test_appconfig.moc"
