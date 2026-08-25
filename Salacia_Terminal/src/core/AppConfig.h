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

    // ---- [log] ----
    QString logDir() const { return logDir_; }
    QString logLevel() const { return logLevel_; }

    // ---- [video] ----
    quint16 videoRtpPort() const { return videoRtpPort_; }
    int jitterLatencyMs() const { return jitterLatencyMs_; }
    QString preferredDecoder() const { return preferredDecoder_; }

    // ---- [ai] ----
    bool aiEnabled() const { return aiEnabled_; }
    QString modelPath() const { return modelPath_; }
    int inputWidth() const { return inputWidth_; }
    int inputHeight() const { return inputHeight_; }
    double confidenceThreshold() const { return confidenceThreshold_; }
    QString executionProvider() const { return executionProvider_; }

    // ---- [rov] ----
    quint16 telemetryPort() const { return telemetryPort_; }
    QString sshHost() const { return sshHost_; }
    quint16 sshPort() const { return sshPort_; }
    QString sshUser() const { return sshUser_; }

private:
    AppConfig() = default;

    static QString findIniFile(const QString& explicitPath);

    bool loaded_ = false;

    // 内置默认值（.ini 缺项或越界时的兜底）
    QString logDir_ = QStringLiteral("logs");
    QString logLevel_ = QStringLiteral("info");

    quint16 videoRtpPort_ = 5000;
    int jitterLatencyMs_ = 20;
    QString preferredDecoder_ = QStringLiteral("d3d11h264dec");

    bool aiEnabled_ = false;
    QString modelPath_ = QStringLiteral("models/model.onnx");
    int inputWidth_ = 640;
    int inputHeight_ = 640;
    double confidenceThreshold_ = 0.5;
    QString executionProvider_ = QStringLiteral("auto");

    quint16 telemetryPort_ = 5001;
    QString sshHost_ = QStringLiteral("192.168.137.2");
    quint16 sshPort_ = 22;
    QString sshUser_ = QStringLiteral("root");
};

} // namespace salacia
