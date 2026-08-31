#pragma once

#include "exwidgets_global.h"

#include <QDialog>

class ExContentDialogPrivate;

/**
 * WinUI 3 ContentDialog 风格的模态对话框（Qt Widgets 实现）。
 *
 * 核心设计：
 *  - 16px padding，8px 圆角，低对比边框
 *  - 标题与内容 12px 间距，内容与按钮区 16px 间距，按钮间距 8px
 *  - 按钮 height=32，minWidth=120，右对齐排列：[ Close ][ Secondary ][ Primary ]
 */
class EXWIDGETS_EXPORT ExContentDialog : public QDialog
{
    Q_OBJECT

public:
    enum ContentDialogResult
    {
        None = 0, ///< CloseButton / ESC / 关闭
        Primary,  ///< PrimaryButton
        Secondary ///< SecondaryButton
    };
    Q_ENUM(ContentDialogResult)

    enum ContentDialogButton
    {
        NoneButton = 0,
        PrimaryBtn,
        SecondaryBtn,
        CloseBtn
    };
    Q_ENUM(ContentDialogButton)

    explicit ExContentDialog(QWidget *parent = nullptr);
    ~ExContentDialog() override;

    QString title() const;
    void setTitle(const QString &title);

    QString content() const;
    void setContent(const QString &text);

    void setContentWidget(QWidget *widget);

    QString primaryButtonText() const;
    void setPrimaryButtonText(const QString &text);

    QString secondaryButtonText() const;
    void setSecondaryButtonText(const QString &text);

    QString closeButtonText() const;
    void setCloseButtonText(const QString &text);

    void setIsPrimaryButtonEnabled(bool enabled);
    void setIsSecondaryButtonEnabled(bool enabled);

    ContentDialogButton defaultButton() const;
    void setDefaultButton(ContentDialogButton button);

    ContentDialogResult showDialog();
    ContentDialogResult result() const;

signals:
    void primaryButtonClicked();
    void secondaryButtonClicked();
    void closeButtonClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    Q_DECLARE_PRIVATE(ExContentDialog)
    ExContentDialogPrivate *d_ptr;
};
