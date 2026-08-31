#pragma once

#include <QFrame>
#include <QWidget>

class QLabel;

namespace salacia {

// 关于页（Phase 5）：软件名/版本/作者/协议版本；未配置项用明确占位（不编造）
class AboutPageWidget : public QFrame
{
    Q_OBJECT

public:
    explicit AboutPageWidget(QWidget* parent = nullptr);

private:
    QLabel* buildRow(const QString& key, const QString& value);
};

} // namespace salacia
