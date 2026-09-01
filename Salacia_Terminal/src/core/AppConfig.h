#pragma once

#include <QString>
#include <QStringList>

#include <atomic>

namespace salacia {

// 全局配置（参数解耦红线：所有可调参数来自 config/app_config.ini）
//
// 线程模型：
//  - load() 仅在主线程、任何工作线程启动之前调用一次；
//  - 加载完成后普通成员只读；标注"实时"的成员为 std::atomic，
//    由设置页经 setter 运行期写入，各线程无锁安全读写。
class AppConfig
{
public:
    static AppConfig& instance();

    // 尝试加载配置：优先 explicitPath，其后依次尝试
    // <可执行文件目录>/config/app_config.ini 与 <工作目录>/config/app_config.ini；
    // 全部缺失时保留内置默认值并返回 false（不视为致命错误）。
    bool load(const QString& explicitPath = QString());

    bool loaded() const { return loaded_; }
    // 实际加载的 ini 绝对路径（未找到配置时为空；设置页"打开配置目录"用）
    QString iniPath() const { return iniPath_; }

    // Logger 初始化后由 main 调用：输出已加载配置摘要
    void logSummary() const;

    // ---- [log] ----
    QString logDir() const { return logDir_; }
    QString logLevel() const { return logLevel_; }

    // ---- [network] ----
    // UDP 绑定地址（视频/遥测接收；空串 = 0.0.0.0 全接口）
    QString hostIp() const { return hostIp_; }
    // 开发板 IP：[tcp] host 留空时的指令通道目标；预留板端定向业务
    QString boardIp() const { return boardIp_; }
    // 绑定地址落地校验：configured 是本机实际地址则原样返回；
    // 否则告警并返回空串（回退 0.0.0.0 全接口，避免绑定失败导致管线错误循环）
    static QString resolveBindAddress(const QString& configured);
    quint16 videoRtpPort() const { return videoRtpPort_; }
    int jitterLatencyMs() const { return jitterLatencyMs_; }
    QString preferredDecoder() const { return preferredDecoder_; }

    // ---- [ai] ----
    bool aiEnabled() const { return aiEnabled_; }
    QString modelPath() const { return modelPath_; }
    // 模型路径落地解析：按原样存在则原样返回；否则尝试 <可执行文件目录>/ 前缀
    //（VS 调试工作目录与 exe 目录不一致时的回退）
    QString resolvedModelPath() const;
    // 类别标签文件（YOLO data.yaml 的 names: 段，或每行一名的纯文本）
    QString labelFile() const { return labelFile_; }
    QString resolvedLabelFile() const;
    int inputWidth() const { return inputWidth_; }
    int inputHeight() const { return inputHeight_; }
    double confidenceThreshold() const { return confidenceThreshold_; }
    float nmsIouThreshold() const { return nmsIouThreshold_; }
    float batteryFullVoltage() const { return batteryFullVoltage_; }
    float batteryEmptyVoltage() const { return batteryEmptyVoltage_; }
    float mahonyKp() const { return mahonyKp_; }
    float mahonyKi() const { return mahonyKi_; }
    QString executionProvider() const { return executionProvider_; }

    // ---- [rov] ----
    quint16 telemetryPort() const { return telemetryPort_; }

    // ---- [control] PWM 量程 ----
    int servoMinUs() const { return servoMinUs_; }
    int servoMaxUs() const { return servoMaxUs_; }
    int thrusterMinUs() const { return thrusterMinUs_; }
    int thrusterMaxUs() const { return thrusterMaxUs_; }
    int thrusterNeutralUs() const { return thrusterNeutralUs_; }

    // ---- [control] 通道与交互（优化任务 Phase 1 迁移）----
    int servoCount() const { return servoCount_; }
    int thrusterCount() const { return thrusterCount_; }
    int controlIdBase() const { return controlIdBase_; }
    int servoMinDeg() const { return servoMinDeg_; }
    int servoMaxDeg() const { return servoMaxDeg_; }
    int servoStepDeg() const { return servoStepDeg_; }
    int thrusterMinPct() const { return thrusterMinPct_; }
    int thrusterMaxPct() const { return thrusterMaxPct_; }
    int thrusterStepPct() const { return thrusterStepPct_; }
    int sliderRateLimitMs() const { return sliderRateLimitMs_; }
    bool releaseFlush() const { return releaseFlush_; }

    // ---- [tcp] Windows<->A35 控制通道（Phase 2 生效，键 Phase 1 就位）----
    // 可用性 = enable 且端口等关键项通过校验；不可用时主程序禁用 TCP 功能并告警
    bool tcpUsable() const { return tcpUsable_; }
    QString tcpHost() const;                 // tcp/host 留空 = [network] board_ip
    quint16 tcpPort() const { return tcpPort_; }
    int connectTimeoutMs() const { return connectTimeoutMs_; }
    int requestTimeoutMs() const { return requestTimeoutMs_; }
    bool heartbeatEnabled() const { return heartbeatEnabled_; }
    int heartbeatIntervalMs() const { return heartbeatIntervalMs_; }
    bool reconnectEnabled() const { return reconnectEnabled_; }
    int reconnectBaseMs() const { return reconnectBaseMs_; }
    int reconnectMaxMs() const { return reconnectMaxMs_; }
    int maxRetry() const { return maxRetry_; }          // 0 = 无限重试
    bool tcpNoDelay() const { return tcpNoDelay_; }
    int recvBufferLimit() const { return recvBufferLimit_; }
    int maxPayload() const { return maxPayload_; }
    int sendQueueCapacity() const { return sendQueueCapacity_; }
    int sensorExpectedHz() const { return sensorExpectedHz_; }
    int sensorStaleMs() const { return sensorStaleMs_; }

    // ---- [network] 遥测兼容回退 ----
    bool telemetryUdpEnabled() const { return telemetryUdpEnabled_; }
    int telemetryWatchdogMs() const { return telemetryWatchdogMs_; }
    int telemetryStaleMs() const { return telemetryStaleMs_; }

    // ---- [battery] 4S 锂电（曲线未标定前显示"待标定"）----
    int cellCount() const { return cellCount_; }
    QString batteryChemistry() const { return batteryChemistry_; }
    QString socCurve() const { return socCurve_; }       // 空 = 待标定
    float socFilterAlpha() const { return socFilterAlpha_; }
    float socHysteresisPct() const { return socHysteresisPct_; }
    float batteryLowThresholdPct() const { return batteryLowPct_; }
    float batteryCriticalThresholdPct() const { return batteryCriticalPct_; }

    // ---- [dyp] DYP-RD 测距 ----
    QString dypUnit() const { return dypUnit_; }
    int dypPrecision() const { return dypPrecision_; }
    float dypValidMin() const { return dypValidMin_; }
    float dypValidMax() const { return dypValidMax_; }
    int dypStaleMs() const { return dypStaleMs_; }
    float dypWarnDistance() const { return dypWarnDistance_; }
    float dypDangerDistance() const { return dypDangerDistance_; }

    // ---- [alarms] 告警中心 ----
    int alarmMaxItems() const { return alarmMaxItems_; }
    int alarmMergeWindowMs() const { return alarmMergeWindowMs_; }
    bool alarmLogEnabled() const { return alarmLogEnabled_; }

    // ---- [ui] 主题/刷新/精度/样式 ----
    QString uiTheme() const { return uiTheme_; }             // light / dark
    QString uiPalette() const { return uiPalette_; }         // fluent / teams
    QString uiStyleName() const { return uiStyleName_; }     // FluentUI3
    QString uiAccentColor() const { return uiAccentColor_; } // 空 = 库默认（Fluent 蓝）
    int textRefreshHz() const { return textRefreshHz_; }
    int attitudeRenderHz() const { return attitudeRenderHz_; }
    int anglePrecision() const { return anglePrecision_; }
    int voltagePrecision() const { return voltagePrecision_; }
    int distancePrecision() const { return distancePrecision_; }
    bool estopConfirmEnabled() const { return estopConfirm_; }
    bool emergencyConfirmEnabled() const { return emergencyConfirm_; }
    int statusMessageShortMs() const { return statusMsgShortMs_; }
    int statusMessageLongMs() const { return statusMsgLongMs_; }
    int statusMessageErrorMs() const { return statusMsgErrorMs_; }
    int windowWidth() const { return windowWidth_; }
    int windowHeight() const { return windowHeight_; }
    int videoRenderIntervalMs() const { return videoRenderIntervalMs_; }
    int commandVideoWidth() const { return commandVideoWidth_; }
    int commandVideoHeight() const { return commandVideoHeight_; }
    float detectLineWidth() const { return detectLineWidth_; }
    float detectLabelFontPt() const { return detectLabelFontPt_; }
    QString estopButtonColor() const { return estopButtonColor_; }
    QString estopButtonHoverColor() const { return estopButtonHoverColor_; }
    int estopButtonMinHeight() const { return estopButtonMinHeight_; }
    QString linkOnlineColor() const { return linkOnlineColor_; }
    QString linkOfflineColor() const { return linkOfflineColor_; }
    int controlNameLabelWidth() const { return controlNameWidth_; }
    int controlValueLabelWidth() const { return controlValueWidth_; }
    QString attitudeBackgroundColor() const { return attitudeBgColor_; }
    int attitudeMinHeight() const { return attitudeMinHeight_; }

    // ---- [system] 线程阶梯与退出 ----
    int workerStopWaitMs() const { return workerStopWaitMs_; }
    int workerInterruptWaitMs() const { return workerInterruptWaitMs_; }
    int workerTerminateWaitMs() const { return workerTerminateWaitMs_; }
    int exitGraceMs() const { return exitGraceMs_; }

    // ---- [video]/[ai]/[log] 运行节拍迁移 ----
    int videoWatchdogIntervalMs() const { return videoWatchdogMs_; }
    int videoStallThresholdMs() const { return videoStallMs_; }
    int videoRestartDelayMs() const { return videoRestartDelayMs_; }
    int videoBusRestartDelayMs() const { return videoBusRestartDelayMs_; }
    int videoNoPacketHintMs() const { return videoNoPacketHintMs_; }
    int videoStatsIntervalMs() const { return videoStatsMs_; }
    int videoSocketBufferBytes() const { return videoSocketBufBytes_; }
    int aiPollIntervalMs() const { return aiPollMs_; }
    int logMaxFileBytes() const { return logMaxFileBytes_; }

    // ---- 实时可调参数 setter（设置页"保存"调用；原子写线程安全）----
    void setConfidenceThreshold(double v);
    void setNmsIouThreshold(double v);
    void setRequestTimeoutMs(int v);
    void setHeartbeatIntervalMs(int v);
    void setSensorStaleMs(int v);
    void setAlarmMaxItems(int v);
    void setAlarmMergeWindowMs(int v);
    void setAnglePrecision(int v);
    void setVoltagePrecision(int v);
    void setDistancePrecision(int v);
    void setBatteryLowPct(double v);
    void setBatteryCriticalPct(double v);

    // 交叉校验问题清单（logSummary 逐条输出；空 = 通过）
    QStringList validationIssues() const { return validationIssues_; }

private:
    AppConfig() = default;

    static QString findIniFile(const QString& explicitPath);
    static QString resolveNearExecutable(const QString& path);

    // 交叉约束校验：问题写入 validationIssues_，并决定 tcpUsable_
    void validate();

    bool loaded_ = false;
    QString iniPath_;

    // 内置默认值（.ini 缺项或越界时的兜底）
    QString logDir_ = QStringLiteral("logs");
    QString logLevel_ = QStringLiteral("info");

    QString hostIp_;
    QString boardIp_ = QStringLiteral("192.168.137.2");
    quint16 videoRtpPort_ = 5000;
    int jitterLatencyMs_ = 40;
    QString preferredDecoder_ = QStringLiteral("d3d11h264dec");

    bool aiEnabled_ = false;
    QString modelPath_ = QStringLiteral("models/model.onnx");
    QString labelFile_; // 空 = 无类别名（框仍显示，退化为"类别 N"）
    int inputWidth_ = 640;
    int inputHeight_ = 640;
    std::atomic<double> confidenceThreshold_{0.5};   // 实时（设置页可调）
    std::atomic<double> nmsIouThreshold_{0.45};       // 实时
    float mahonyKp_ = 0.5F;
    float mahonyKi_ = 0.0F;
    QString executionProvider_ = QStringLiteral("auto");

    quint16 telemetryPort_ = 5001;
    float batteryFullVoltage_ = 16.8F;   // 满电电压（默认 4S 锂电）
    float batteryEmptyVoltage_ = 13.0F;  // 放空电压

    int servoMinUs_ = 500;        // 舵机 0° 脉宽
    int servoMaxUs_ = 2500;       // 舵机 180° 脉宽
    int thrusterMinUs_ = 1100;    // 推进器满倒脉宽
    int thrusterMaxUs_ = 1900;    // 推进器满顺脉宽
    int thrusterNeutralUs_ = 1500; // 推进器中位脉宽

    // [control] 通道与交互
    int servoCount_ = 10;
    int thrusterCount_ = 6;
    int controlIdBase_ = 1;
    int servoMinDeg_ = 0;
    int servoMaxDeg_ = 180;
    int servoStepDeg_ = 1;
    int thrusterMinPct_ = -100;
    int thrusterMaxPct_ = 100;
    int thrusterStepPct_ = 1;
    int sliderRateLimitMs_ = 50;
    bool releaseFlush_ = true;

    // [tcp]
    bool tcpEnable_ = false;
    bool tcpUsable_ = false;
    QString tcpHostOverride_;     // [tcp] host 原值（空 = 用 board_ip）
    quint16 tcpPort_ = 7000;
    int connectTimeoutMs_ = 3000;
    std::atomic<int> requestTimeoutMs_{1000};        // 实时
    bool heartbeatEnabled_ = true;
    std::atomic<int> heartbeatIntervalMs_{1000};     // 实时
    bool reconnectEnabled_ = true;
    int reconnectBaseMs_ = 1000;
    int reconnectMaxMs_ = 10000;
    int maxRetry_ = 0;
    bool tcpNoDelay_ = true;
    int recvBufferLimit_ = 65536;
    int maxPayload_ = 4096;
    int sendQueueCapacity_ = 64;
    int sensorExpectedHz_ = 100;
    std::atomic<int> sensorStaleMs_{500};            // 实时

    // [network] 遥测兼容回退
    bool telemetryUdpEnabled_ = true;
    int telemetryWatchdogMs_ = 500;
    int telemetryStaleMs_ = 1000;

    // [battery]
    int cellCount_ = 4;
    QString batteryChemistry_;
    QString socCurve_;
    float socFilterAlpha_ = 0.2F;
    float socHysteresisPct_ = 3.0F;
    std::atomic<double> batteryLowPct_{30.0};        // 实时
    std::atomic<double> batteryCriticalPct_{15.0};   // 实时

    // [dyp]
    QString dypUnit_ = QStringLiteral("mm");
    int dypPrecision_ = 0;
    float dypValidMin_ = 20.0F;
    float dypValidMax_ = 8000.0F;
    int dypStaleMs_ = 500;
    float dypWarnDistance_ = 1000.0F;
    float dypDangerDistance_ = 300.0F;

    // [alarms]
    std::atomic<int> alarmMaxItems_{200};            // 实时
    std::atomic<int> alarmMergeWindowMs_{5000};      // 实时
    bool alarmLogEnabled_ = true;

    // [ui]
    QString uiTheme_ = QStringLiteral("light");
    QString uiPalette_ = QStringLiteral("fluent");
    QString uiStyleName_ = QStringLiteral("FluentUI3");
    QString uiAccentColor_; // 空 = 库默认；#RRGGBB 自定义强调色
    int textRefreshHz_ = 5;
    int attitudeRenderHz_ = 20;
    std::atomic<int> anglePrecision_{1};             // 实时
    std::atomic<int> voltagePrecision_{2};           // 实时
    std::atomic<int> distancePrecision_{0};          // 实时
    bool estopConfirm_ = false;
    bool emergencyConfirm_ = true;
    int statusMsgShortMs_ = 3000;
    int statusMsgLongMs_ = 5000;
    int statusMsgErrorMs_ = 10000;
    int windowWidth_ = 1440;
    int windowHeight_ = 860;
    int videoRenderIntervalMs_ = 33;
    int commandVideoWidth_ = 320;  // 指令页小视频尺寸
    int commandVideoHeight_ = 180;
    float detectLineWidth_ = 2.0F;
    float detectLabelFontPt_ = 9.5F;
    QString estopButtonColor_ = QStringLiteral("#8c2f2f");
    QString estopButtonHoverColor_ = QStringLiteral("#a83a3a");
    int estopButtonMinHeight_ = 42;
    QString linkOnlineColor_ = QStringLiteral("#3ddc84");
    QString linkOfflineColor_ = QStringLiteral("#c0a040");
    int controlNameWidth_ = 64;
    int controlValueWidth_ = 110;
    QString attitudeBgColor_ = QStringLiteral("#141922");
    int attitudeMinHeight_ = 340;

    // [system]
    int workerStopWaitMs_ = 3000;
    int workerInterruptWaitMs_ = 2000;
    int workerTerminateWaitMs_ = 1000;
    int exitGraceMs_ = 500;

    // [video]/[ai]/[log] 节拍
    int videoWatchdogMs_ = 500;
    int videoStallMs_ = 2000;
    int videoRestartDelayMs_ = 100;
    int videoBusRestartDelayMs_ = 500;
    int videoNoPacketHintMs_ = 5000;
    int videoStatsMs_ = 1000;
    int videoSocketBufBytes_ = 2097152;
    int aiPollMs_ = 5;
    int logMaxFileBytes_ = 10 * 1024 * 1024;

    QStringList validationIssues_;
};

} // namespace salacia
