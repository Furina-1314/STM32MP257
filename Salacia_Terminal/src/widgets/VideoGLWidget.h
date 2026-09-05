#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLWidget>

#include <memory>

#include "video/VideoFrame.h"
#include "video/VideoFrameHub.h"

class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;
class QTimer;

namespace salacia {

// 视频显示组件（QOpenGLWidget + OpenGL 渲染，红线：解码/渲染由 GPU 承担）
//
// 数据通路：GStreamer 解码线程(生产者) -> VideoFrameHub 最新帧发布层 ->
// 本组件(GUI 线程)快照。多个实例（主页/指令页小画面）共享同一 Hub，
// 互不竞争、零拷贝（shared_ptr 快照）。
// 拉取节奏：33ms 定时器驱动，仅在有新帧（帧序号变化）时才请求重绘。
//
// AI 检测叠加：paintGL 内经 DataManager 读写锁读取最新检测集合，
// 在视频视口上以 NDC 线框绘制检测框（归一化坐标与画面同源，无需换算）。
//
// 分辨率适配：1080p/720p 双格式动态切换——纹理尺寸变化时重建，
// 画面按源宽高比信箱式(letterbox)居中显示。
class VideoGLWidget : public QOpenGLWidget, private QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit VideoGLWidget(QWidget* parent = nullptr);
    ~VideoGLWidget() override;

    // 绑定最新帧发布层（不拥有；生命周期由 GStreamerPipeline 保证：
    // MainWindow 关闭时先停管线再析构）
    void setSource(VideoFrameHub* hub);

    quint64 renderedFrames() const { return renderedFrames_; }

    // 提前显式释放 GL 资源并排空 GPU（closeEvent 首步调用；幂等）。
    // 带流退出时 d3d11 拆卸与 GL 末帧在驱动内部并发会触发 ICD 崩溃
    // （实测 igxelpicd64.dll 0xC0000005），先停绘制并 glFinish 消除竞态。
    void releaseGl();

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    void drainLatest();            // 快照最新帧（序号未变则保持上一画面）
    void drawDetections();         // 检测框叠加（NDC 线框，含类别配色）

    VideoFrameHub* source_ = nullptr; // 不拥有

    std::unique_ptr<QOpenGLShaderProgram> program_;     // 视频纹理着色
    std::unique_ptr<QOpenGLShaderProgram> lineProgram_; // 检测框纯色着色
    std::unique_ptr<QOpenGLBuffer> vertexBuffer_;
    std::unique_ptr<QOpenGLBuffer> lineBuffer_;  // 检测框动态顶点
    // 独立 VAO：视频四边形与检测框各自的属性绑定，杜绝共享默认 VAO
    // 的属性指针互相踩踏（曾引发 Intel ICD 运行期崩溃）
    std::unique_ptr<QOpenGLVertexArrayObject> vaoQuad_;
    std::unique_ptr<QOpenGLVertexArrayObject> vaoLines_;
    unsigned int textureId_ = 0;  // OpenGL 纹理句柄（GL 资源，dtor 中释放）
    int textureWidth_ = 0;
    int textureHeight_ = 0;

    std::shared_ptr<const VideoFrame> frame_; // 最新帧快照（GUI 线程私有，共享只读）
    quint64 lastFrameIndex_ = 0;              // 已渲染的帧序号
    bool hasNewFrame_ = false;
    QTimer* repaintTimer_ = nullptr;
    quint64 renderedFrames_ = 0;
    bool glCleaned_ = false; // releaseGl 幂等标记
};

} // namespace salacia
