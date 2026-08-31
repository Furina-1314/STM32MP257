#include "VideoGLWidget.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QPen>
#include <QTimer>

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "core/AppConfig.h"
#include "core/DataManager.h"

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

// 检测框纯色着色（仅位置属性）
constexpr const char* kLineVertexShader =
    "attribute vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }";

constexpr const char* kLineFragmentShader =
    "uniform vec4 uColor;\n"
    "void main() { gl_FragColor = uColor; }";

// 类别配色表（classId 取模轮转，高亮绿开头）
constexpr float kPalette[][4] = {
    {0.20F, 1.00F, 0.20F, 1.0F}, // 绿
    {0.25F, 0.65F, 1.00F, 1.0F}, // 蓝
    {1.00F, 0.55F, 0.15F, 1.0F}, // 橙
    {1.00F, 0.25F, 0.55F, 1.0F}, // 品红
    {1.00F, 0.95F, 0.20F, 1.0F}, // 黄
    {0.70F, 0.40F, 1.00F, 1.0F}, // 紫
    {0.20F, 0.95F, 0.90F, 1.0F}, // 青
    {1.00F, 1.00F, 1.00F, 1.0F}, // 白
};
constexpr int kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

// 类别配色：前 8 类用固定调色板，其后按黄金角旋转色相（保证逐类别互异）
QColor classColor(int classId)
{
    if ((classId >= 0) && (classId < kPaletteSize)) {
        return QColor(qRound(kPalette[classId][0] * 255.0F),
                      qRound(kPalette[classId][1] * 255.0F),
                      qRound(kPalette[classId][2] * 255.0F));
    }
    const int index = (classId > 0) ? classId : 0;
    const qreal hue = std::fmod((index - kPaletteSize) * 137.508, 360.0);
    return QColor::fromHsvF(hue / 360.0, 0.90, 1.0);
}
} // namespace

VideoGLWidget::VideoGLWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setMinimumSize(QSize(320, 240));
    repaintTimer_ = new QTimer(this);
    repaintTimer_->setInterval(AppConfig::instance().videoRenderIntervalMs());
    connect(repaintTimer_, &QTimer::timeout, this, [this] {
        if ((source_ != nullptr) && !source_->empty()) {
            update(); // 仅有新帧时请求重绘
        }
    });
    repaintTimer_->start();
}

VideoGLWidget::~VideoGLWidget()
{
    if (!glCleaned_) {
        // GL 资源必须在上下文存续期间释放（先于智能指针成员析构）
        makeCurrent();
        if (textureId_ != 0U) {
            glDeleteTextures(1, &textureId_);
            textureId_ = 0;
        }
        program_.reset();
        lineProgram_.reset();
        vaoQuad_.reset();
        vaoLines_.reset();
        vertexBuffer_.reset();
        lineBuffer_.reset();
        doneCurrent();
    }
}

void VideoGLWidget::releaseGl()
{
    if (glCleaned_) {
        return; // 幂等
    }
    glCleaned_ = true;

    // 1) 停止拉取与重绘（不再产生新的 GL 工作）
    repaintTimer_->stop();
    source_ = nullptr;

    // 2) 释放全部 GL 资源并排空 GPU 队列（上下文仍完全健康的窗口期）
    makeCurrent();
    if (textureId_ != 0U) {
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
    }
    program_.reset();
    lineProgram_.reset();
    vaoQuad_.reset();
    vaoLines_.reset();
    vertexBuffer_.reset();
    lineBuffer_.reset();
    glFinish(); // 等 GPU 队列清空，消除与 d3d11 拆卸的驱动层竞态
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

    lineProgram_ = std::make_unique<QOpenGLShaderProgram>();
    lineProgram_->addShaderFromSourceCode(QOpenGLShader::Vertex, kLineVertexShader);
    lineProgram_->addShaderFromSourceCode(QOpenGLShader::Fragment, kLineFragmentShader);
    lineProgram_->link();

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

    lineBuffer_ = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
    lineBuffer_->create();
    lineBuffer_->bind();
    lineBuffer_->allocate(nullptr, 5 * 2 * sizeof(float));

    // 视频四边形 VAO：属性绑定封存于独立 VAO，绘制时整体切换。
    // setAttributeBuffer 记录的是"当时全局绑定的 GL_ARRAY_BUFFER"——
    // 必须先把本 VAO 对应的 buffer 绑上，否则属性会指向最后绑定的
    // lineBuffer_（未初始化内存），四边形退化为不可见（黑屏根因）
    vaoQuad_ = std::make_unique<QOpenGLVertexArrayObject>();
    vaoQuad_->create();
    vaoQuad_->bind();
    program_->bind();
    vertexBuffer_->bind();
    program_->enableAttributeArray("aPos");
    program_->setAttributeBuffer("aPos", GL_FLOAT, 0, 2, 4 * sizeof(float));
    program_->enableAttributeArray("aUv");
    program_->setAttributeBuffer("aUv", GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));
    program_->release();
    vaoQuad_->release();

    // 检测框 VAO（同理：记录属性前显式绑定 lineBuffer_）
    vaoLines_ = std::make_unique<QOpenGLVertexArrayObject>();
    vaoLines_->create();
    vaoLines_->bind();
    lineProgram_->bind();
    lineBuffer_->bind();
    lineProgram_->enableAttributeArray("aPos");
    lineProgram_->setAttributeBuffer("aPos", GL_FLOAT, 0, 2, 2 * sizeof(float));
    lineProgram_->release();
    vaoLines_->release();

    vertexBuffer_->release();
    lineBuffer_->release();

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
    vaoQuad_->bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    vaoQuad_->release();
    program_->release();

    drawDetections(); // 检测框叠加（同视口，归一化坐标直映 NDC）

    ++renderedFrames_;
}

void VideoGLWidget::drawDetections()
{
    // UI 线程经读写锁读取最新检测（DataManager 低频写/UI 高频读设计）
    const std::vector<Detection> dets = DataManager::instance().detections();
    if (dets.empty()) {
        return;
    }
    const std::vector<QString> classNames = DataManager::instance().classNames();

    // 与 paintGL 相同的信箱几何：检测框归一化坐标（图像左上原点）须先
    // 映射到视频实际显示矩形，再转 NDC，否则宽高比不一致时框会越出画面
    const qreal dpr = devicePixelRatioF();
    const int viewW = static_cast<int>(width() * dpr);
    const int viewH = static_cast<int>(height() * dpr);
    const double sourceAspect = (frame_.height > 0)
            ? static_cast<double>(frame_.width) / frame_.height
            : static_cast<double>(viewW) / viewH;
    const double viewAspect = static_cast<double>(viewW) / viewH;
    int dstW = viewW;
    int dstH = viewH;
    if (sourceAspect > viewAspect) {
        dstH = static_cast<int>(viewW / sourceAspect);
    } else {
        dstW = static_cast<int>(viewH * sourceAspect);
    }
    const double offX = (viewW - dstW) / 2.0;
    const double offY = (viewH - dstH) / 2.0;
    const auto toNdcX = [&](double nx) { return (offX + nx * dstW) / viewW * 2.0 - 1.0; };
    const auto toNdcY = [&](double ny) { return 1.0 - (offY + ny * dstH) / viewH * 2.0; };

    // ---- GL 线框（逐类别独立配色）----
    glLineWidth(AppConfig::instance().detectLineWidth());
    lineProgram_->bind();
    vaoLines_->bind();
    lineBuffer_->bind(); // 供 write() 更新顶点，属性关联由 VAO 持有

    for (const Detection& d : dets) {
        const float x1 = static_cast<float>(toNdcX(d.x));
        const float y1 = static_cast<float>(toNdcY(d.y));
        const float x2 = static_cast<float>(toNdcX(d.x + d.w));
        const float y2 = static_cast<float>(toNdcY(d.y + d.h));
        const float verts[5][2] = {
            {x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}, {x1, y1},
        };

        const QColor color = classColor(d.classId);
        lineProgram_->setUniformValue("uColor",
                                      static_cast<float>(color.redF()),
                                      static_cast<float>(color.greenF()),
                                      static_cast<float>(color.blueF()), 1.0F);
        lineBuffer_->write(0, verts, sizeof(verts));
        glDrawArrays(GL_LINE_STRIP, 0, 5);
    }

    lineBuffer_->release();
    vaoLines_->release();
    lineProgram_->release();

    // ---- 标签框（QPainter，逻辑像素；名称 + 置信度，随类别同色）----
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSizeF(AppConfig::instance().detectLabelFontPt()); // 点长与 DPI 无关
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const int pad = 3;

    const double scaleL = 1.0 / dpr; // 设备像素 -> 逻辑像素
    const double dstWl = dstW * scaleL;
    const double dstHl = dstH * scaleL;
    const double offXl = offX * scaleL;
    const double offYl = offY * scaleL;

    for (const Detection& d : dets) {
        const int cid = (d.classId >= 0) ? d.classId : 0;
        QString name = (static_cast<std::size_t>(cid) < classNames.size())
                ? classNames[static_cast<std::size_t>(cid)] : QString();
        if (name.isEmpty()) {
            name = QString::fromLocal8Bit("类别 %1").arg(cid);
        }
        const QString text = QStringLiteral("%1 %2%")
                .arg(name)
                .arg(qRound(d.confidence * 100.0F));

        const QColor color = classColor(d.classId);
        const int textW = metrics.horizontalAdvance(text);
        const int boxH = metrics.height() + pad * 2;
        const int boxW = textW + pad * 2;

        // 标签锚在检测框左上角；越出窗口顶部时翻到框内侧
        double px = offXl + d.x * dstWl;
        double py = offYl + d.y * dstHl;
        int labelX = static_cast<int>(px);
        int labelY = static_cast<int>(py) - boxH - 1;
        if (labelY < 0) {
            labelY = static_cast<int>(py) + 1;
        }
        if (labelX + boxW > width()) {
            labelX = width() - boxW - 1;
        }
        if (labelX < 0) {
            labelX = 0;
        }

        // 亮色填充配深色字，深色填充配白字（保证对比度）
        const int lum = (color.red() * 299 + color.green() * 587 + color.blue() * 114) / 1000;
        const QColor textColor = (lum > 150) ? QColor(20, 20, 20) : QColor(255, 255, 255);

        painter.setPen(QPen(color, 1));
        painter.setBrush(color);
        painter.drawRect(labelX, labelY, boxW, boxH);
        painter.setPen(textColor);
        painter.setBrush(Qt::NoBrush);
        painter.drawText(QRect(labelX + pad, labelY + pad, textW, metrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
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
