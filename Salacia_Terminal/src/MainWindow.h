#pragma once

#include <QMainWindow>

#include <memory>

class QLabel;
class QQuickWidget;

namespace salacia {

class GStreamerPipeline;
class OnnxInferEngine;
class UdpReceiver;
class RovVizModel;
class VideoGLWidget;

// 主窗口（Widgets 框架红线）
//
// 结构：中央视频区（含 AI 检测框叠加）；右侧"舱体状态"坞 =
// Quick3D 三维姿态视图（QQuickWidget，OpenGL RHI 全局强制）+
// 传感器表单（姿态欧拉角/舱内温湿度/电池电量/遥测状态）；
// 状态栏常驻视频/AI/遥测三组统计。
// 生命周期（逆序安全退出红线）：closeEvent 先停视频绘制并排空 GPU，
// 再停网络接收（视频 + 遥测），再停推理线程（工作线程释放
// ONNX/GPU 上下文），最后进入 GUI 析构。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QWidget* createStatusDock(); // 右侧坞：3D 姿态 + 传感器表单

    std::unique_ptr<GStreamerPipeline> pipeline_;
    std::unique_ptr<UdpReceiver> telemetryReceiver_; // 无父（Worker 红线）
    std::unique_ptr<OnnxInferEngine> aiEngine_;      // 仅在 aiEnabled 时创建
    RovVizModel* rovViz_ = nullptr;                  // Qt 父子所有权（主线程）

    VideoGLWidget* videoWidget_ = nullptr; // Qt 父子所有权（退出时脱离防 ICD 崩溃）
    QLabel* videoStatsLabel_ = nullptr;
    QLabel* aiStatsLabel_ = nullptr;
    QLabel* telemetryLabel_ = nullptr;

    // 传感器表单标签（5Hz 节流刷新）
    QLabel* rollLabel_ = nullptr;
    QLabel* pitchLabel_ = nullptr;
    QLabel* yawLabel_ = nullptr;
    QLabel* tempLabel_ = nullptr;
    QLabel* humidLabel_ = nullptr;
    QLabel* batteryLabel_ = nullptr;
    qint64 lastPanelMs_ = 0;
    qint64 lastAiLabelMs_ = 0;
};

} // namespace salacia
