#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLWidget>

#include <memory>

#include "video/VideoFrame.h"
#include "utils/RingBuffer.h"

class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;
class QTimer;

namespace salacia {

// 视频显示组件（QOpenGLWidget + OpenGL 渲染，红线：解码/渲染由 GPU 承担）
//
// 数据通路：GStreamer 解码线程(生产者) -> 无锁 RingBuffer -> 本组件(GUI 线程)
// 拉取节奏：33ms 定时器驱动（720p@30fps 场景满帧刷新；1080p@12fps 同步覆盖），
// 仅在有新帧时才请求重绘，空转零开销。
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

    // 绑定显示帧通道（不拥有；生命周期由 GStreamerPipeline 保证：
    // MainWindow 关闭时先停管线再析构）
    void setSource(RingBuffer<VideoFrame, 4>* ring);

    quint64 renderedFrames() const { return renderedFrames_; }

    // 提前显式释放 GL 资源并排空 GPU（closeEvent 首步调用；幂等）。
    // 带流退出时 d3d11 拆卸与 GL 末帧在驱动内部并发会触发 ICD 崩溃
    // （实测 igxelpicd64.dll 0xC0000005），先停绘制并 glFinish 消除竞态。
    void releaseGl();

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    void drainLatest();            // 排空环形缓冲，仅保留最新帧（旧帧丢弃保低延迟）
    void drawDetections();         // 检测框叠加（NDC 线框，含类别配色）

    RingBuffer<VideoFrame, 4>* source_ = nullptr; // 不拥有

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

    VideoFrame frame_;      // 最新帧缓存（GUI 线程私有）
    bool hasNewFrame_ = false;
    QTimer* repaintTimer_ = nullptr;
    quint64 renderedFrames_ = 0;
    bool glCleaned_ = false; // releaseGl 幂等标记
};

} // namespace salacia
