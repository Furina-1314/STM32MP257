#pragma once

#include <QMainWindow>

#include <memory>

class QLabel;

namespace salacia {

class GStreamerPipeline;
class VideoGLWidget;

// 主窗口（Widgets 框架红线）
//
// 结构：视频显示组件为中央控件；状态栏常驻视频链路统计（fps/丢帧/在线）。
// 生命周期（逆序安全退出红线）：closeEvent 先停视频数据面（管线），
// 随后 GUI 逐层析构；pipeline_ 声明于 videoWidget_ 之前，
// 保证成员析构顺序为 标签 -> 管线（含 RingBuffer）-> 子控件。
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
    VideoGLWidget* videoWidget_ = nullptr; // Qt 父子所有权（非裸资源）
    QLabel* videoStatsLabel_ = nullptr;    // Qt 父子所有权
};

} // namespace salacia
