#include "AboutPageWidget.h"

#include <QFrame>
#include <QLabel>
#include <QPalette>
#include <QVBoxLayout>

#include <QtGlobal>

namespace salacia {

AboutPageWidget::AboutPageWidget(QWidget* parent)
    : QFrame(parent)
{
    // Gallery AboutProjectWidget 卡片式布局：QFrame + 卡片 + 标题/副标题/富文本
    setFrameShape(QFrame::StyledPanel);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(9, 9, 9, 9);
    layout->setSpacing(0);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("aboutCard"));
    card->setFrameShape(QFrame::StyledPanel);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(9, 9, 9, 9);
    cardLayout->setSpacing(10);

    auto* titleLabel = new QLabel(QString::fromLocal8Bit("关于"), card);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    auto* subtitleLabel = new QLabel(
            QString::fromLocal8Bit("Salacia_Terminal · ROV 水下机器人岸上终端"), card);
    QPalette subtitlePalette = subtitleLabel->palette();
    subtitlePalette.setColor(QPalette::WindowText,
                             subtitlePalette.color(QPalette::Mid));
    subtitleLabel->setPalette(subtitlePalette);

    auto* contentLabel = new QLabel(card);
    contentLabel->setWordWrap(true);
    contentLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    QFont contentFont = contentLabel->font();
    contentFont.setPointSize(11);
    contentLabel->setFont(contentFont);
    contentLabel->setTextFormat(Qt::RichText);

    // 项目元数据无单一事实来源（构建元数据未配置）——以明确占位符列出，不编造
#ifdef NDEBUG
    const char* buildType = "Release";
#else
    const char* buildType = "Debug";
#endif
    contentLabel->setText(QString::fromLocal8Bit(
            "<p><b>软件名称：</b>Salacia_Terminal（ROV 岸上终端）</p>"
            "<p><b>软件版本：</b>未配置（待提供，构建元数据就位后接入）</p>"
            "<p><b>作者：</b>未配置（待提供）</p>"
            "<p><b>Windows-A35 协议版本：</b>TCP wire 草案 v1（待 A35 确认）</p>"
            "<p><b>构建信息：</b>%1 / Qt %2 / %3-bit</p>"
            "<p><b>界面库：</b>FluentUIStyle-3.0 (MIT) + QWindowKit 1.5.0 (Apache-2.0)</p>"
            "<p>功能：实时视频（1080p/720p RTP/H264）、ONNX 识别（DirectML）、"
            "100Hz 传感器汇总（TCP）+ 遥测回退（UDP）、三维姿态显示（Quick3D）、"
            "16 路 PWM 执行机构遥控（10 舵机 + 6 推进器）、FluentUI 界面。</p>")
            .arg(QString::fromLatin1(buildType),
                 QString::fromLatin1(QT_VERSION_STR),
                 QString::number(static_cast<int>(sizeof(void*)) * 8)));

    cardLayout->addWidget(titleLabel);
    cardLayout->addWidget(subtitleLabel);
    cardLayout->addWidget(contentLabel, 1);
    layout->addWidget(card, 1);
}

} // namespace salacia
