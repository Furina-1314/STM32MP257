#pragma once

#include <QMainWindow>

#include <memory>

class ExWinUINavigationView;
class FluentTitleBar;
class QLabel;
class QPushButton;
class QQuickWidget;
class QStackedWidget;

namespace salacia {

class AboutPageWidget;
class AlarmBarWidget;
class CommandPageWidget;
class AlarmModel;
class ControlAreaWidget;
class ControlViewModel;
class GStreamerPipeline;
class OnnxInferEngine;
class RovVizModel;
class SafetyStateModel;
class SensorModel;
class SettingsPageWidget;
class TcpClient;
class UdpReceiver;
class VideoGLWidget;

// FluentUIStyle 主窗口（导航页式布局）
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QWidget* createHomePage();
    void startDataFaces();
    void connectTcpFace();
    void requestEmergencyWithConfirm();

    std::unique_ptr<GStreamerPipeline> pipeline_;
    std::unique_ptr<UdpReceiver> telemetryReceiver_; // 无父（Worker 红线）
    std::unique_ptr<TcpClient> tcpClient_;           // 无父（Worker 红线）
    std::unique_ptr<SensorModel> sensorModel_;       // 主线程数据模型
    std::unique_ptr<OnnxInferEngine> aiEngine_;      // 仅在 aiEnabled 时创建
    RovVizModel* rovViz_ = nullptr;                  // Qt 父子所有权（主线程）

    std::unique_ptr<AlarmModel> alarmModel_;
    std::unique_ptr<SafetyStateModel> safety_;
    std::unique_ptr<ControlViewModel> controlVm_;

    // ---- 窗口骨架 ----
    FluentTitleBar* titleBar_ = nullptr;
    ExWinUINavigationView* nav_ = nullptr;
    QPushButton* navToggleBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    AlarmBarWidget* alarmBar_ = nullptr;
    CommandPageWidget* commandPage_ = nullptr;
    SettingsPageWidget* settingsPage_ = nullptr;
    AboutPageWidget* aboutPage_ = nullptr;

    // ---- 主页 ----
    VideoGLWidget* videoWidget_ = nullptr;
    QQuickWidget* quick_ = nullptr;
    QLabel* rollLabel_ = nullptr;
    QLabel* pitchLabel_ = nullptr;
    QLabel* yawLabel_ = nullptr;
    QLabel* tempLabel_ = nullptr;
    QLabel* humidLabel_ = nullptr;
    QLabel* batteryLabel_ = nullptr;
    QLabel* dypLabel_ = nullptr;
    QLabel* sensorFreshLabel_ = nullptr;
    qint64 lastPanelMs_ = 0;
    qint64 lastAiLabelMs_ = 0;

    ControlAreaWidget* controlArea_ = nullptr;

    // ---- 状态栏 ----
    QLabel* videoStatsLabel_ = nullptr;
    QLabel* aiStatsLabel_ = nullptr;
    QLabel* telemetryLabel_ = nullptr;
    QLabel* tcpLabel_ = nullptr;
};

} // namespace salacia
