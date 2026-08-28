#pragma once

#include <QString>

namespace salacia {

// 全局配置（参数解耦红线：所有可调参数来自 config/app_config.ini）
//
// 线程模型：
//  - load() 仅在主线程、任何工作线程启动之前调用一次；
//  - 加载完成后全部成员只读，各线程经 const 访问器读取，
//    无锁安全（load-once / read-many）；如需运行期热更新，
//    须重构为 std::shared_mutex 读写锁版本（当前不做）。
class AppConfig
{
public:
    static AppConfig& instance();

    // 尝试加载配置：优先 explicitPath，其后依次尝试
    // <可执行文件目录>/config/app_config.ini 与 <工作目录>/config/app_config.ini；
    // 全部缺失时保留内置默认值并返回 false（不视为致命错误）。
    bool load(const QString& explicitPath = QString());

    bool loaded() const { return loaded_; }

    // Logger 初始化后由 main 调用：输出已加载配置摘要
    void logSummary() const;

    // ---- [log] ----
    QString logDir() const { return logDir_; }
    QString logLevel() const { return logLevel_; }

    // ---- [network] ----
    // UDP 绑定地址（视频/遥测接收；空串 = 0.0.0.0 全接口）
    QString hostIp() const { return hostIp_; }
    // 开发板 IP（ssh_host 留空时的指令通道目标；预留板端定向业务）
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
    // SSH 目标主机：[rov] ssh_host 优先，留空回退 [network] board_ip
    QString sshHost() const;
    quint16 sshPort() const { return sshPort_; }
    QString sshUser() const { return sshUser_; }
    QString sshPassword() const { return sshPassword_; }
    QString sshKeyPath() const { return sshKeyPath_; }
    int sshReconnectSec() const { return sshReconnectSec_; }

    // ---- [control] PWM 量程 ----
    int servoMinUs() const { return servoMinUs_; }
    int servoMaxUs() const { return servoMaxUs_; }
    int thrusterMinUs() const { return thrusterMinUs_; }
    int thrusterMaxUs() const { return thrusterMaxUs_; }
    int thrusterNeutralUs() const { return thrusterNeutralUs_; }

private:
    AppConfig() = default;

    static QString findIniFile(const QString& explicitPath);

    bool loaded_ = false;

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
    int inputWidth_ = 640;
    int inputHeight_ = 640;
    double confidenceThreshold_ = 0.5;
    float nmsIouThreshold_ = 0.45F;
    float mahonyKp_ = 0.5F;
    float mahonyKi_ = 0.0F;
    QString executionProvider_ = QStringLiteral("auto");

    quint16 telemetryPort_ = 5001;
    float batteryFullVoltage_ = 16.8F;   // 满电电压（默认 4S 锂电）
    float batteryEmptyVoltage_ = 13.0F;  // 放空电压
    QString sshHostOverride_;            // [rov] ssh_host 原值（空 = 用 board_ip）
    quint16 sshPort_ = 22;
    QString sshUser_ = QStringLiteral("root");
    QString sshPassword_;
    QString sshKeyPath_;
    int sshReconnectSec_ = 5;

    int servoMinUs_ = 500;        // 舵机 0° 脉宽
    int servoMaxUs_ = 2500;       // 舵机 180° 脉宽
    int thrusterMinUs_ = 1100;    // 推进器满倒脉宽
    int thrusterMaxUs_ = 1900;    // 推进器满顺脉宽
    int thrusterNeutralUs_ = 1500; // 推进器中位脉宽
};

} // namespace salacia
