#include "AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QSettings>
#include <QVariant>
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
    confidenceThreshold_.store(boundedDouble(ini, "ai/confidence_threshold",
                                              confidenceThreshold_.load(), 0.0, 1.0));
    nmsIouThreshold_.store(boundedDouble(ini, "ai/nms_iou_threshold",
                                         nmsIouThreshold_.load(), 0.0, 1.0));
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

    // ---- [control] ----
    servoMinUs_ = boundedInt(ini, "control/servo_min_us", servoMinUs_, 500, 3000);
    servoMaxUs_ = boundedInt(ini, "control/servo_max_us", servoMaxUs_, 500, 3000);
    thrusterMinUs_ = boundedInt(ini, "control/thruster_min_us", thrusterMinUs_, 800, 2200);
    thrusterMaxUs_ = boundedInt(ini, "control/thruster_max_us", thrusterMaxUs_, 800, 2200);
    thrusterNeutralUs_ = boundedInt(ini, "control/thruster_neutral_us", thrusterNeutralUs_, 800, 2200);
    servoCount_ = boundedInt(ini, "control/servo_count", servoCount_, 1, 32);
    thrusterCount_ = boundedInt(ini, "control/thruster_count", thrusterCount_, 1, 32);
    controlIdBase_ = boundedInt(ini, "control/id_base", controlIdBase_, 0, 255);
    // [弃用] 执行器拓扑以 WireConstants 为唯一权威（舵机 wire 0-9、垂直 10-13、
    // 水平 14-15）；三键仅为旧 ini 兼容保留，存在即告警，不影响运行时行为
    if (ini.contains(QStringLiteral("control/servo_count"))
        || ini.contains(QStringLiteral("control/thruster_count"))
        || ini.contains(QStringLiteral("control/id_base"))) {
        Logger::warning(QString::fromLocal8Bit(
                "control/servo_count、thruster_count、id_base 已弃用："
                "执行器拓扑以协议常量为准（舵机 wire 0-9，垂直 CH10-13，"
                "水平 CH14-15）"));
    }
    servoMinDeg_ = boundedInt(ini, "control/servo_min_deg", servoMinDeg_, 0, 180);
    servoMaxDeg_ = boundedInt(ini, "control/servo_max_deg", servoMaxDeg_, 0, 180);
    servoStepDeg_ = boundedInt(ini, "control/servo_step_deg", servoStepDeg_, 1, 90);
    thrusterMinPct_ = boundedInt(ini, "control/thruster_min_pct", thrusterMinPct_, -100, 0);
    thrusterMaxPct_ = boundedInt(ini, "control/thruster_max_pct", thrusterMaxPct_, 0, 100);
    thrusterStepPct_ = boundedInt(ini, "control/thruster_step_pct", thrusterStepPct_, 1, 100);
    sliderRateLimitMs_ = boundedInt(ini, "control/slider_rate_limit_ms", sliderRateLimitMs_, 10, 1000);
    releaseFlush_ = ini.value(QStringLiteral("control/release_flush"), releaseFlush_).toBool();

    // ---- [tcp] ----
    tcpEnable_ = ini.value(QStringLiteral("tcp/enable"), false).toBool();
    tcpHostOverride_ = ini.value(QStringLiteral("tcp/host"), tcpHostOverride_).toString().trimmed();
    tcpPort_ = static_cast<quint16>(boundedInt(ini, "tcp/port", tcpPort_, 1, 65535));
    connectTimeoutMs_ = boundedInt(ini, "tcp/connect_timeout_ms", connectTimeoutMs_, 100, 60000);
    requestTimeoutMs_.store(boundedInt(ini, "tcp/request_timeout_ms",
                                       requestTimeoutMs_.load(), 100, 60000));
    heartbeatEnabled_ = ini.value(QStringLiteral("tcp/heartbeat_enable"), heartbeatEnabled_).toBool();
    heartbeatIntervalMs_.store(boundedInt(ini, "tcp/heartbeat_interval_ms",
                                          heartbeatIntervalMs_.load(), 100, 60000));
    reconnectEnabled_ = ini.value(QStringLiteral("tcp/reconnect_enable"), reconnectEnabled_).toBool();
    reconnectBaseMs_ = boundedInt(ini, "tcp/reconnect_base_ms", reconnectBaseMs_, 100, 60000);
    reconnectMaxMs_ = boundedInt(ini, "tcp/reconnect_max_ms", reconnectMaxMs_, 100, 600000);
    maxRetry_ = boundedInt(ini, "tcp/max_retry", maxRetry_, 0, 1000);
    tcpNoDelay_ = ini.value(QStringLiteral("tcp/tcp_nodelay"), tcpNoDelay_).toBool();
    recvBufferLimit_ = boundedInt(ini, "tcp/recv_buffer_limit", recvBufferLimit_, 1024, 1048576);
    maxPayload_ = boundedInt(ini, "tcp/max_payload", maxPayload_, 64, 65535);
    sendQueueCapacity_ = boundedInt(ini, "tcp/send_queue_capacity", sendQueueCapacity_, 8, 1024);
    sensorExpectedHz_ = boundedInt(ini, "tcp/sensor_expected_hz", sensorExpectedHz_, 1, 1000);
    sensorStaleMs_.store(boundedInt(ini, "tcp/sensor_stale_ms",
                                    sensorStaleMs_.load(), 50, 10000));

    // ---- [network] 遥测兼容回退 ----
    telemetryUdpEnabled_ = ini.value(QStringLiteral("network/telemetry_udp_enable"),
                                     telemetryUdpEnabled_).toBool();
    telemetryWatchdogMs_ = boundedInt(ini, "network/telemetry_watchdog_ms",
                                      telemetryWatchdogMs_, 100, 10000);
    telemetryStaleMs_ = boundedInt(ini, "network/telemetry_stale_ms",
                                   telemetryStaleMs_, 100, 30000);

    // ---- [battery] ----
    cellCount_ = boundedInt(ini, "battery/cell_count", cellCount_, 1, 16);
    batteryChemistry_ = ini.value(QStringLiteral("battery/chemistry"),
                                  batteryChemistry_).toString().trimmed();
    {
        // QSettings IniFormat 会把含逗号的值解析为 QStringList（toString 得空串），
        // 统一 join 回空格分隔；推荐写法为空格分隔（见 ini 注释）
        const QVariant curveVar = ini.value(QStringLiteral("battery/soc_curve"), socCurve_);
        if (curveVar.metaType().id() == QMetaType::QStringList) {
            socCurve_ = curveVar.toStringList().join(QLatin1Char(' ')).trimmed();
        } else {
            socCurve_ = curveVar.toString().trimmed();
        }
    }
    socFilterAlpha_ = static_cast<float>(boundedDouble(ini, "battery/soc_filter_alpha",
                                                       socFilterAlpha_, 0.01, 1.0));
    socHysteresisPct_ = static_cast<float>(boundedDouble(ini, "battery/soc_hysteresis_pct",
                                                         socHysteresisPct_, 0.0, 50.0));
    batteryLowPct_.store(boundedDouble(ini, "battery/low_threshold_pct",
                                       batteryLowPct_.load(), 0.0, 100.0));
    batteryCriticalPct_.store(boundedDouble(ini, "battery/critical_threshold_pct",
                                            batteryCriticalPct_.load(), 0.0, 100.0));

    // ---- [dyp] ----
    dypUnit_ = ini.value(QStringLiteral("dyp/unit"), dypUnit_).toString().trimmed();
    dypPrecision_ = boundedInt(ini, "dyp/precision", dypPrecision_, 0, 3);
    dypValidMin_ = static_cast<float>(boundedDouble(ini, "dyp/valid_min_mm", dypValidMin_, 0.0, 10000.0));
    dypValidMax_ = static_cast<float>(boundedDouble(ini, "dyp/valid_max_mm", dypValidMax_, 1.0, 100000.0));
    dypStaleMs_ = boundedInt(ini, "dyp/stale_ms", dypStaleMs_, 50, 10000);
    dypWarnDistance_ = static_cast<float>(boundedDouble(ini, "dyp/warn_distance_mm",
                                                        dypWarnDistance_, 0.0, 100000.0));
    dypDangerDistance_ = static_cast<float>(boundedDouble(ini, "dyp/danger_distance_mm",
                                                          dypDangerDistance_, 0.0, 100000.0));

    // ---- [alarms] ----
    alarmMaxItems_.store(boundedInt(ini, "alarms/max_items",
                                    alarmMaxItems_.load(), 10, 10000));
    alarmMergeWindowMs_.store(boundedInt(ini, "alarms/merge_window_ms",
                                         alarmMergeWindowMs_.load(), 0, 600000));
    alarmLogEnabled_ = ini.value(QStringLiteral("alarms/log_alarms"), alarmLogEnabled_).toBool();
    alarmPanelMaxHeight_ = boundedInt(ini, "alarms/panel_max_height",
                                      alarmPanelMaxHeight_, 100, 600);

    // ---- [ui] ----
    uiTheme_ = ini.value(QStringLiteral("ui/theme"), uiTheme_).toString().toLower();
    uiPalette_ = ini.value(QStringLiteral("ui/palette"), uiPalette_).toString().toLower();
    uiStyleName_ = ini.value(QStringLiteral("ui/style"), uiStyleName_).toString();
    uiAccentColor_ = ini.value(QStringLiteral("ui/accent_color"),
                               uiAccentColor_).toString().trimmed();
    textRefreshHz_ = boundedInt(ini, "ui/text_refresh_hz", textRefreshHz_, 1, 60);
    attitudeRenderHz_ = boundedInt(ini, "ui/attitude_render_hz", attitudeRenderHz_, 1, 60);
    anglePrecision_.store(boundedInt(ini, "ui/angle_precision",
                                     anglePrecision_.load(), 0, 3));
    voltagePrecision_.store(boundedInt(ini, "ui/voltage_precision",
                                       voltagePrecision_.load(), 0, 3));
    distancePrecision_.store(boundedInt(ini, "ui/distance_precision",
                                        distancePrecision_.load(), 0, 3));
    estopConfirm_ = ini.value(QStringLiteral("ui/estop_confirm"), estopConfirm_).toBool();
    emergencyConfirm_ = ini.value(QStringLiteral("ui/emergency_confirm"), emergencyConfirm_).toBool();
    statusMsgShortMs_ = boundedInt(ini, "ui/status_message_short_ms", statusMsgShortMs_, 500, 60000);
    statusMsgLongMs_ = boundedInt(ini, "ui/status_message_long_ms", statusMsgLongMs_, 500, 60000);
    statusMsgErrorMs_ = boundedInt(ini, "ui/status_message_error_ms", statusMsgErrorMs_, 500, 60000);
    windowWidth_ = boundedInt(ini, "ui/window_width", windowWidth_, 640, 7680);
    windowHeight_ = boundedInt(ini, "ui/window_height", windowHeight_, 480, 4320);
    videoRenderIntervalMs_ = boundedInt(ini, "ui/video_render_interval_ms",
                                        videoRenderIntervalMs_, 10, 200);
    commandVideoWidth_ = boundedInt(ini, "ui/command_video_width",
                                    commandVideoWidth_, 160, 800);
    commandVideoHeight_ = boundedInt(ini, "ui/command_video_height",
                                     commandVideoHeight_, 90, 600);
    detectLineWidth_ = static_cast<float>(boundedDouble(ini, "ui/detect_line_width",
                                                        detectLineWidth_, 0.5, 10.0));
    detectLabelFontPt_ = static_cast<float>(boundedDouble(ini, "ui/detect_label_font_pt",
                                                          detectLabelFontPt_, 6.0, 24.0));
    estopButtonColor_ = ini.value(QStringLiteral("ui/estop_button_color"), estopButtonColor_).toString();
    estopButtonHoverColor_ = ini.value(QStringLiteral("ui/estop_button_hover_color"),
                                       estopButtonHoverColor_).toString();
    estopButtonMinHeight_ = boundedInt(ini, "ui/estop_button_min_height", estopButtonMinHeight_, 20, 200);
    linkOnlineColor_ = ini.value(QStringLiteral("ui/link_online_color"), linkOnlineColor_).toString();
    linkOfflineColor_ = ini.value(QStringLiteral("ui/link_offline_color"), linkOfflineColor_).toString();
    controlNameWidth_ = boundedInt(ini, "ui/control_name_label_width", controlNameWidth_, 20, 400);
    controlValueWidth_ = boundedInt(ini, "ui/control_value_label_width", controlValueWidth_, 40, 600);
    attitudeBgColor_ = ini.value(QStringLiteral("ui/attitude_background_color"),
                                 attitudeBgColor_).toString();
    attitudeMinHeight_ = boundedInt(ini, "ui/attitude_min_height", attitudeMinHeight_, 120, 2000);

    // ---- [system] ----
    workerStopWaitMs_ = boundedInt(ini, "system/worker_stop_wait_ms", workerStopWaitMs_, 100, 30000);
    workerInterruptWaitMs_ = boundedInt(ini, "system/worker_interrupt_wait_ms",
                                        workerInterruptWaitMs_, 100, 30000);
    workerTerminateWaitMs_ = boundedInt(ini, "system/worker_terminate_wait_ms",
                                        workerTerminateWaitMs_, 100, 30000);
    exitGraceMs_ = boundedInt(ini, "system/exit_grace_ms", exitGraceMs_, 0, 10000);

    // ---- [video]/[ai]/[log] 节拍 ----
    videoWatchdogMs_ = boundedInt(ini, "video/watchdog_interval_ms", videoWatchdogMs_, 100, 10000);
    videoStallMs_ = boundedInt(ini, "video/stall_threshold_ms", videoStallMs_, 500, 30000);
    videoRestartDelayMs_ = boundedInt(ini, "video/restart_delay_ms", videoRestartDelayMs_, 10, 10000);
    videoBusRestartDelayMs_ = boundedInt(ini, "video/bus_restart_delay_ms",
                                         videoBusRestartDelayMs_, 10, 10000);
    videoNoPacketHintMs_ = boundedInt(ini, "video/no_packet_hint_ms", videoNoPacketHintMs_, 1000, 60000);
    videoStatsMs_ = boundedInt(ini, "video/stats_interval_ms", videoStatsMs_, 100, 10000);
    videoSocketBufBytes_ = boundedInt(ini, "video/socket_buffer_bytes", videoSocketBufBytes_,
                                      65536, 16777216);
    aiPollMs_ = boundedInt(ini, "ai/poll_interval_ms", aiPollMs_, 1, 1000);
    logMaxFileBytes_ = boundedInt(ini, "log/max_file_bytes", logMaxFileBytes_, 65536, 104857600);

    validate();

    iniPath_ = path;
    loaded_ = true;
    return true;
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

QString AppConfig::tcpHost() const
{
    return tcpHostOverride_.isEmpty() ? boardIp_ : tcpHostOverride_;
}

void AppConfig::validate()
{
    validationIssues_.clear();
    const auto add = [this](const QString& issue) { validationIssues_ << issue; };

    if (reconnectMaxMs_ < reconnectBaseMs_) {
        add(QStringLiteral("tcp/reconnect_max_ms (%1) < tcp/reconnect_base_ms (%2)")
                .arg(reconnectMaxMs_).arg(reconnectBaseMs_));
    }
    if (servoMinDeg_ >= servoMaxDeg_) {
        add(QStringLiteral("control/servo_min_deg (%1) >= servo_max_deg (%2)")
                .arg(servoMinDeg_).arg(servoMaxDeg_));
    }
    if (thrusterMinPct_ >= thrusterMaxPct_) {
        add(QStringLiteral("control/thruster_min_pct (%1) >= thruster_max_pct (%2)")
                .arg(thrusterMinPct_).arg(thrusterMaxPct_));
    }
    if (batteryLowPct_ <= batteryCriticalPct_) {
        add(QStringLiteral("battery/low_threshold_pct (%1) <= critical_threshold_pct (%2)")
                .arg(batteryLowPct_.load()).arg(batteryCriticalPct_.load()));
    }
    if (dypValidMin_ >= dypValidMax_) {
        add(QStringLiteral("dyp/valid_min_mm (%1) >= valid_max_mm (%2)")
                .arg(dypValidMin_).arg(dypValidMax_));
    }
    if (dypWarnDistance_ <= dypDangerDistance_) {
        add(QStringLiteral("dyp/warn_distance_mm (%1) <= danger_distance_mm (%2)")
                .arg(dypWarnDistance_).arg(dypDangerDistance_));
    }
    if ((uiTheme_ != QStringLiteral("light")) && (uiTheme_ != QStringLiteral("dark"))) {
        add(QString::fromLocal8Bit("ui/theme (%1) 非法（light/dark）").arg(uiTheme_));
    }
    if ((uiPalette_ != QStringLiteral("fluent")) && (uiPalette_ != QStringLiteral("teams"))) {
        add(QString::fromLocal8Bit("ui/palette (%1) 非法（fluent/teams）").arg(uiPalette_));
    }

    // TCP 可用性：enable 且无 [tcp] 相关键问题（端口/载荷上限已在 bounded* 越界回退）
    tcpUsable_ = tcpEnable_ && validationIssues_.isEmpty();
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
    Logger::info(QString::fromLocal8Bit("AppConfig: 已加载（主机 %1，板端 %2，视频端口 %3，抖动缓冲 %4ms，解码器 %5，AI=%6 [%7 %8x%9 阈值%10]，遥测端口 %11）")
                     .arg(hostIp_.isEmpty() ? QStringLiteral("0.0.0.0") : hostIp_)
                     .arg(boardIp_)
                     .arg(videoRtpPort_)
                     .arg(jitterLatencyMs_)
                     .arg(preferredDecoder_)
                     .arg(aiEnabled_ ? QStringLiteral("ON") : QStringLiteral("OFF"))
                     .arg(executionProvider_)
                     .arg(inputWidth_)
                     .arg(inputHeight_)
                     .arg(confidenceThreshold_.load())
                     .arg(telemetryPort_));

    // 交叉校验结果（缺关键键/冲突值：TCP 功能禁用降级红线）
    if (tcpEnable_ && !tcpUsable_) {
        Logger::error(QString::fromLocal8Bit("AppConfig: [tcp] 校验未通过，TCP 控制通道已禁用："));
        for (const QString& issue : validationIssues_) {
            Logger::error(QString::fromLocal8Bit("AppConfig:   - %1").arg(issue));
        }
    } else if (!validationIssues_.isEmpty()) {
        for (const QString& issue : validationIssues_) {
            Logger::warning(QString::fromLocal8Bit("AppConfig: 交叉校验警告：%1").arg(issue));
        }
    }
    Logger::info(QString::fromLocal8Bit("AppConfig: TCP=%1（%2:%3，遥测 UDP 回退=%4，主题 %5/%6）")
                     .arg(tcpUsable_ ? QString::fromLatin1("ON") : QString::fromLatin1("OFF"))
                     .arg(tcpHost())
                     .arg(tcpPort_)
                     .arg(telemetryUdpEnabled_ ? QString::fromLatin1("ON") : QString::fromLatin1("OFF"))
                     .arg(uiTheme_, uiPalette_));
}

// ------------------------------------------------ 实时可调参数 setter

void AppConfig::setConfidenceThreshold(double v) { confidenceThreshold_.store(v); }
void AppConfig::setNmsIouThreshold(double v) { nmsIouThreshold_.store(v); }
void AppConfig::setRequestTimeoutMs(int v) { requestTimeoutMs_.store(v); }
void AppConfig::setHeartbeatIntervalMs(int v) { heartbeatIntervalMs_.store(v); }
void AppConfig::setSensorStaleMs(int v) { sensorStaleMs_.store(v); }
void AppConfig::setAlarmMaxItems(int v) { alarmMaxItems_.store(v); }
void AppConfig::setAlarmMergeWindowMs(int v) { alarmMergeWindowMs_.store(v); }
void AppConfig::setAnglePrecision(int v) { anglePrecision_.store(v); }
void AppConfig::setVoltagePrecision(int v) { voltagePrecision_.store(v); }
void AppConfig::setDistancePrecision(int v) { distancePrecision_.store(v); }
void AppConfig::setBatteryLowPct(double v) { batteryLowPct_.store(v); }
void AppConfig::setBatteryCriticalPct(double v) { batteryCriticalPct_.store(v); }

} // namespace salacia
