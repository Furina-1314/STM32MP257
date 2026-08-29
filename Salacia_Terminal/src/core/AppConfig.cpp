#include "AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QSettings>
#include <QStringList>

#include "Logger.h"

namespace salacia {

namespace {

// 带边界校验的整数读取（网络输入/配置输入边界校验红线）
int boundedInt(const QSettings& ini, const char* key, int defaultValue, int lo, int hi)
{
    bool ok = false;
    const int value = ini.value(QLatin1String(key), defaultValue).toInt(&ok);
    if (!ok || value < lo || value > hi) {
        if (Logger::isInitialized()) {
            Logger::warning(QString::fromLocal8Bit("AppConfig: 配置项 %1 缺失或越界（%2~%3），回退默认值 %4")
                                .arg(QString::fromLatin1(key))
                                .arg(lo)
                                .arg(hi)
                                .arg(defaultValue));
        }
        return defaultValue;
    }
    return value;
}

// 带边界校验的浮点读取
double boundedDouble(const QSettings& ini, const char* key, double defaultValue,
                     double lo, double hi)
{
    bool ok = false;
    const double value = ini.value(QLatin1String(key), defaultValue).toDouble(&ok);
    if (!ok || value < lo || value > hi) {
        if (Logger::isInitialized()) {
            Logger::warning(QString::fromLocal8Bit("AppConfig: 配置项 %1 缺失或越界（%2~%3），回退默认值 %4")
                                .arg(QString::fromLatin1(key))
                                .arg(lo)
                                .arg(hi)
                                .arg(defaultValue));
        }
        return defaultValue;
    }
    return value;
}

} // namespace

AppConfig& AppConfig::instance()
{
    static AppConfig config; // Meyers 单例：主线程首次 load() 时构造
    return config;
}

QString AppConfig::findIniFile(const QString& explicitPath)
{
    QStringList candidates;
    if (!explicitPath.isEmpty()) {
        candidates << explicitPath;
    }
    candidates << QCoreApplication::applicationDirPath() + QStringLiteral("/config/app_config.ini")
               << QDir::currentPath() + QStringLiteral("/config/app_config.ini");

    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

bool AppConfig::load(const QString& explicitPath)
{
    const QString path = findIniFile(explicitPath);
    if (path.isEmpty()) {
        if (Logger::isInitialized()) {
            Logger::warning(QString::fromLocal8Bit("AppConfig: 未找到 app_config.ini，全部使用内置默认值"));
        }
        return false;
    }

    QSettings ini(path, QSettings::IniFormat);

    // ---- [log] ----
    logDir_ = ini.value(QStringLiteral("log/dir"), logDir_).toString();
    logLevel_ = ini.value(QStringLiteral("log/level"), logLevel_).toString().toLower();

    // ---- [network] ----
    hostIp_ = ini.value(QStringLiteral("network/host_ip"), hostIp_).toString().trimmed();
    boardIp_ = ini.value(QStringLiteral("network/board_ip"), boardIp_).toString().trimmed();
    videoRtpPort_ = static_cast<quint16>(boundedInt(ini, "network/rtp_port",
                                                    videoRtpPort_, 1, 65535));
    jitterLatencyMs_ = boundedInt(ini, "video/jitter_latency_ms",
                                  jitterLatencyMs_, 0, 1000);
    preferredDecoder_ = ini.value(QStringLiteral("video/preferred_decoder"),
                                  preferredDecoder_).toString();

    // ---- [ai] ----
    aiEnabled_ = ini.value(QStringLiteral("ai/enable"), false).toBool();
    modelPath_ = ini.value(QStringLiteral("ai/model_path"), modelPath_).toString();
    labelFile_ = ini.value(QStringLiteral("ai/label_file"), labelFile_).toString().trimmed();
    inputWidth_ = boundedInt(ini, "ai/input_width", inputWidth_, 16, 8192);
    inputHeight_ = boundedInt(ini, "ai/input_height", inputHeight_, 16, 8192);
    confidenceThreshold_ = boundedDouble(ini, "ai/confidence_threshold",
                                         confidenceThreshold_, 0.0, 1.0);
    nmsIouThreshold_ = static_cast<float>(boundedDouble(ini, "ai/nms_iou_threshold",
                                                       nmsIouThreshold_, 0.0, 1.0));
    mahonyKp_ = static_cast<float>(boundedDouble(ini, "imu/mahony_kp", mahonyKp_, 0.0, 10.0));
    mahonyKi_ = static_cast<float>(boundedDouble(ini, "imu/mahony_ki", mahonyKi_, 0.0, 10.0));
    executionProvider_ = ini.value(QStringLiteral("ai/execution_provider"),
                                   executionProvider_).toString().toLower();

    // ---- [rov] ----
    telemetryPort_ = static_cast<quint16>(boundedInt(ini, "network/telemetry_port",
                                                     telemetryPort_, 1, 65535));
    batteryFullVoltage_ = static_cast<float>(boundedDouble(ini, "rov/battery_full_voltage",
                                                           batteryFullVoltage_, 3.0, 60.0));
    batteryEmptyVoltage_ = static_cast<float>(boundedDouble(ini, "rov/battery_empty_voltage",
                                                            batteryEmptyVoltage_, 3.0, 60.0));
    sshHostOverride_ = ini.value(QStringLiteral("rov/ssh_host"),
                                 sshHostOverride_).toString().trimmed();
    sshPort_ = static_cast<quint16>(boundedInt(ini, "rov/ssh_port",
                                               sshPort_, 1, 65535));
    sshUser_ = ini.value(QStringLiteral("rov/ssh_user"), sshUser_).toString();
    sshPassword_ = ini.value(QStringLiteral("rov/ssh_password"), sshPassword_).toString();
    sshKeyPath_ = ini.value(QStringLiteral("rov/ssh_key_path"), sshKeyPath_).toString();
    sshReconnectSec_ = boundedInt(ini, "rov/ssh_reconnect_sec", sshReconnectSec_, 1, 60);

    // ---- [control] ----
    servoMinUs_ = boundedInt(ini, "control/servo_min_us", servoMinUs_, 500, 3000);
    servoMaxUs_ = boundedInt(ini, "control/servo_max_us", servoMaxUs_, 500, 3000);
    thrusterMinUs_ = boundedInt(ini, "control/thruster_min_us", thrusterMinUs_, 800, 2200);
    thrusterMaxUs_ = boundedInt(ini, "control/thruster_max_us", thrusterMaxUs_, 800, 2200);
    thrusterNeutralUs_ = boundedInt(ini, "control/thruster_neutral_us", thrusterNeutralUs_, 800, 2200);

    loaded_ = true;
    return true;
}


QString AppConfig::sshHost() const
{
    return sshHostOverride_.isEmpty() ? boardIp_ : sshHostOverride_;
}

QString AppConfig::resolveNearExecutable(const QString& path)
{
    if (path.isEmpty() || QFileInfo::exists(path)) {
        return path;
    }
    const QString nearExe = QCoreApplication::applicationDirPath()
            + QLatin1Char('/') + path;
    return QFileInfo::exists(nearExe) ? nearExe : path;
}

QString AppConfig::resolvedModelPath() const
{
    return resolveNearExecutable(modelPath_);
}

QString AppConfig::resolvedLabelFile() const
{
    return resolveNearExecutable(labelFile_);
}

QString AppConfig::resolveBindAddress(const QString& configured)
{
    if (configured.isEmpty()) {
        return QString();
    }
    const QList<QHostAddress> locals = QNetworkInterface::allAddresses();
    for (const QHostAddress& addr : locals) {
        if (addr == QHostAddress{configured}) {
            return configured;
        }
    }
    if (Logger::isInitialized()) {
        Logger::warning(QString::fromLocal8Bit("AppConfig: [network] host_ip=%1 不是本机任何网卡地址"
                                               "（回退 0.0.0.0 全接口监听；请核对 ICS/NAT 是否启用或修正配置）")
                            .arg(configured));
    }
    return QString();
}

void AppConfig::logSummary() const
{
    if (!Logger::isInitialized()) {
        return;
    }
    Logger::info(QString::fromLocal8Bit("AppConfig: 已加载（主机 %1，板端 %2，视频端口 %3，抖动缓冲 %4ms，解码器 %5，AI=%6 [%7 %8x%9 阈值%10]，遥测端口 %11，SSH %12:%13@%14）")
                     .arg(hostIp_.isEmpty() ? QStringLiteral("0.0.0.0") : hostIp_)
                     .arg(boardIp_)
                     .arg(videoRtpPort_)
                     .arg(jitterLatencyMs_)
                     .arg(preferredDecoder_)
                     .arg(aiEnabled_ ? QStringLiteral("ON") : QStringLiteral("OFF"))
                     .arg(executionProvider_)
                     .arg(inputWidth_)
                     .arg(inputHeight_)
                     .arg(confidenceThreshold_)
                     .arg(telemetryPort_)
                     .arg(sshHost())
                     .arg(sshPort_)
                     .arg(sshUser_));
}

} // namespace salacia
