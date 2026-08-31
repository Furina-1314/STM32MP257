#pragma once

#include <QWidget>

#include "core/AlarmModel.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace salacia {

// 顶部告警中心（完整实现）
//
// 结构：摘要条（最高相关性告警，Error 不被 Info 覆盖）+ 展开按钮 +
//       可滚动列表（级别/时间/来源/摘要/详情/Seq/状态）+ 三级筛选。
//
// 线程安全：AlarmModel 互斥；alarmsChanged（自动排队）驱动刷新——UI 仅主线程触碰。
class AlarmBarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AlarmBarWidget(AlarmModel* model, QWidget* parent = nullptr);

public slots:
    void refresh();          // AlarmModel::alarmsChanged -> 此处（QueuedConnection）
    void toggleExpanded();   // 摘要条点击 / 展开按钮

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    quint8 filterMask() const;
    void rebuildTable();

    AlarmModel* model_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QPushButton* expandBtn_ = nullptr;
    QWidget* panel_ = nullptr;
    QCheckBox* infoCb_ = nullptr;
    QCheckBox* warnCb_ = nullptr;
    QCheckBox* errorCb_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QTableWidget* table_ = nullptr;
    bool expanded_ = false;
};

} // namespace salacia
