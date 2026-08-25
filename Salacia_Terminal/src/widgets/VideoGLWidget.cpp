#include "VideoGLWidget.h"

#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QTimer>

#include <cstring>
#include <utility>

namespace salacia {

namespace {
// 兼容性 GLSL（无版本声明，attribute/varying 语法），适配默认兼容性上下文
constexpr const char* kVertexShader =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUv;\n"
    "varying vec2 vUv;\n"
    "void main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }";

constexpr const char* kFragmentShader =
    "uniform sampler2D tex;\n"
    "varying vec2 vUv;\n"
    "void main() { gl_FragColor = texture2D(tex, vUv); }";
} // namespace

VideoGLWidget::VideoGLWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setMinimumSize(QSize(320, 240));
    repaintTimer_ = new QTimer(this);
    repaintTimer_->setInterval(33); // 约 30Hz 拉取（覆盖 1080p@12 与 720p@30）
    connect(repaintTimer_, &QTimer::timeout, this, [this] {
        if ((source_ != nullptr) && !source_->empty()) {
            update(); // 仅有新帧时请求重绘
        }
    });
    repaintTimer_->start();
}

VideoGLWidget::~VideoGLWidget()
{
    // GL 资源必须在上下文存续期间释放（先于智能指针成员析构）
    makeCurrent();
    if (textureId_ != 0U) {
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
    }
    program_.reset();
    vertexBuffer_.reset();
    doneCurrent();
}

void VideoGLWidget::setSource(RingBuffer<VideoFrame, 4>* ring)
{
    source_ = ring;
}

void VideoGLWidget::initializeGL()
{
    initializeOpenGLFunctions();

    program_ = std::make_unique<QOpenGLShaderProgram>();
    program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    program_->link();

    // 交错顶点：位置(x,y) + 纹理(u,v)，三角形带四角
    constexpr float kQuad[] = {
        // x      y     u     v
        -1.0F, -1.0F, 0.0F, 1.0F,
         1.0F, -1.0F, 1.0F, 1.0F,
        -1.0F,  1.0F, 0.0F, 0.0F,
         1.0F,  1.0F, 1.0F, 0.0F,
    };
    vertexBuffer_ = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
    vertexBuffer_->create();
    vertexBuffer_->bind();
    vertexBuffer_->allocate(kQuad, sizeof(kQuad));

    program_->bind();
    program_->enableAttributeArray("aPos");
    program_->setAttributeBuffer("aPos", GL_FLOAT, 0, 2, 4 * sizeof(float));
    program_->enableAttributeArray("aUv");
    program_->setAttributeBuffer("aUv", GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));
    program_->release();

    glGenTextures(1, &textureId_);
    glBindTexture(GL_TEXTURE_2D, textureId_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
}

void VideoGLWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    drainLatest();
    if ((frame_.width <= 0) || (frame_.height <= 0) || frame_.data.empty()) {
        return; // 尚无帧：保持黑底
    }

    // 纹理：尺寸变化时重建（1080p <-> 720p 切换），随后增量更新
    glBindTexture(GL_TEXTURE_2D, textureId_);
    if (hasNewFrame_ || (textureWidth_ != frame_.width) || (textureHeight_ != frame_.height)) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame_.width, frame_.height, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, frame_.data.data());
        textureWidth_ = frame_.width;
        textureHeight_ = frame_.height;
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    // 信箱式居中：按源宽高比计算视口矩形（设备像素）
    const qreal dpr = devicePixelRatioF();
    const int viewW = static_cast<int>(width() * dpr);
    const int viewH = static_cast<int>(height() * dpr);
    const double sourceAspect = static_cast<double>(frame_.width) / frame_.height;
    const double viewAspect = static_cast<double>(viewW) / viewH;
    int dstW = viewW;
    int dstH = viewH;
    if (sourceAspect > viewAspect) {
        dstH = static_cast<int>(viewW / sourceAspect);
    } else {
        dstW = static_cast<int>(viewH * sourceAspect);
    }
    glViewport((viewW - dstW) / 2, (viewH - dstH) / 2, dstW, dstH);

    program_->bind();
    program_->setUniformValue("tex", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId_);
    vertexBuffer_->bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_->release();

    ++renderedFrames_;
}

void VideoGLWidget::drainLatest()
{
    VideoFrame incoming;
    bool got = false;
    while ((source_ != nullptr) && source_->pop(incoming)) {
        frame_ = std::move(incoming); // 排空，仅保留最新（旧帧丢弃保低延迟）
        got = true;
    }
    hasNewFrame_ = got;
}

} // namespace salacia
