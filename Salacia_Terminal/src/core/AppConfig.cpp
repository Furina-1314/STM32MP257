#include "AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
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

    // ---- [video] ----
    videoRtpPort_ = static_cast<quint16>(boundedInt(ini, "video/rtp_port",
                                                    videoRtpPort_, 1, 65535));
    jitterLatencyMs_ = boundedInt(ini, "video/jitter_latency_ms",
                                  jitterLatencyMs_, 0, 1000);
    preferredDecoder_ = ini.value(QStringLiteral("video/preferred_decoder"),
                                  preferredDecoder_).toString();

    // ---- [ai] ----
    aiEnabled_ = ini.value(QStringLiteral("ai/enable"), false).toBool();
    modelPath_ = ini.value(QStringLiteral("ai/model_path"), modelPath_).toString();
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
    telemetryPort_ = static_cast<quint16>(boundedInt(ini, "rov/telemetry_port",
                                                     telemetryPort_, 1, 65535));
    batteryFullVoltage_ = static_cast<float>(boundedDouble(ini, "rov/battery_full_voltage",
                                                           batteryFullVoltage_, 3.0, 60.0));
    batteryEmptyVoltage_ = static_cast<float>(boundedDouble(ini, "rov/battery_empty_voltage",
                                                            batteryEmptyVoltage_, 3.0, 60.0));
    sshHost_ = ini.value(QStringLiteral("rov/ssh_host"), sshHost_).toString();
    sshPort_ = static_cast<quint16>(boundedInt(ini, "rov/ssh_port",
                                               sshPort_, 1, 65535));
    sshUser_ = ini.value(QStringLiteral("rov/ssh_user"), sshUser_).toString();

    loaded_ = true;
    return true;
}


void AppConfig::logSummary() const
{
    if (!Logger::isInitialized()) {
        return;
    }
    Logger::info(QString::fromLocal8Bit("AppConfig: 已加载（视频端口 %1，抖动缓冲 %2ms，解码器 %3，AI=%4 [%5 %6x%7 阈值%8]，遥测端口 %9，SSH %10:%11@%12）")
                     .arg(videoRtpPort_)
                     .arg(jitterLatencyMs_)
                     .arg(preferredDecoder_)
                     .arg(aiEnabled_ ? QStringLiteral("ON") : QStringLiteral("OFF"))
                     .arg(executionProvider_)
                     .arg(inputWidth_)
                     .arg(inputHeight_)
                     .arg(confidenceThreshold_)
                     .arg(telemetryPort_)
                     .arg(sshHost_)
                     .arg(sshPort_)
                     .arg(sshUser_));
}

} // namespace salacia
