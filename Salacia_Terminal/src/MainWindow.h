#pragma once

#include <QMainWindow>

#include <memory>

class QLabel;

namespace salacia {

class GStreamerPipeline;
class OnnxInferEngine;
class VideoGLWidget;

// 主窗口（Widgets 框架红线）
//
// 结构：视频显示组件（含 AI 检测框叠加）为中央控件；状态栏常驻
// 视频链路统计（fps/丢帧/在线）与 AI 状态（后端/推理耗时/目标数）。
// 生命周期（逆序安全退出红线）：closeEvent 先停视频数据面（网络接收），
// 再停推理线程（其内部最后释放 ONNX/GPU 上下文），随后 GUI 逐层析构。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    std::unique_ptr<GStreamerPipeline> pipeline_;
    std::unique_ptr<OnnxInferEngine> aiEngine_; // 仅在 aiEnabled 时创建
    VideoGLWidget* videoWidget_ = nullptr;      // Qt 父子所有权（非裸资源）
    QLabel* videoStatsLabel_ = nullptr;         // Qt 父子所有权
    QLabel* aiStatsLabel_ = nullptr;            // Qt 父子所有权
    qint64 lastAiLabelMs_ = 0;                  // AI 状态标签节流（200ms）
};

} // namespace salacia
