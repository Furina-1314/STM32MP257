#include "AlarmBarWidget.h"

#include <QCheckBox>
#include <QDateTime>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QVBoxLayout>

#include "core/AppConfig.h"

namespace salacia {

namespace {
QString levelText(AlarmLevel level)
{
    switch (level) {
    case AlarmLevel::Error:   return QString::fromLocal8Bit("错误");
    case AlarmLevel::Warning: return QString::fromLocal8Bit("警告");
    default:                  return QString::fromLocal8Bit("信息");
    }
}

const char* levelColor(AlarmLevel level)
{
    switch (level) {
    case AlarmLevel::Error:   return "#dc2626";
    case AlarmLevel::Warning: return "#d97706";
    default:                  return "#2563eb";
    }
}
} // namespace

AlarmBarWidget::AlarmBarWidget(AlarmModel* model, QWidget* parent)
    : QWidget(parent)
    , model_(model)
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(2);

    // ---- 摘要条 ----
    auto* bar = new QWidget(this);
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(6, 2, 6, 2);
    summaryLabel_ = new QLabel(QString::fromLocal8Bit("告警：无"), bar);
    summaryLabel_->setMinimumHeight(24);
    expandBtn_ = new QPushButton(QString::fromLocal8Bit("展开"), bar);
    expandBtn_->setFlat(true);
    expandBtn_->setFixedWidth(72);
    barLayout->addWidget(summaryLabel_, 1);
    barLayout->addWidget(expandBtn_);
    rootLayout->addWidget(bar);
    summaryLabel_->installEventFilter(this); // 点击摘要 = 切换展开

    // ---- 展开面板（筛选 + 列表）----
    panel_ = new QWidget(this);
    auto* panelLayout = new QVBoxLayout(panel_);
    panelLayout->setContentsMargins(4, 2, 4, 2);

    auto* filterRow = new QHBoxLayout();
    infoCb_ = new QCheckBox(QString::fromLocal8Bit("信息"), panel_);
    warnCb_ = new QCheckBox(QString::fromLocal8Bit("警告"), panel_);
    errorCb_ = new QCheckBox(QString::fromLocal8Bit("错误"), panel_);
    infoCb_->setChecked(true);
    warnCb_->setChecked(true);
    errorCb_->setChecked(true);
    clearBtn_ = new QPushButton(QString::fromLocal8Bit("清空"), panel_);
    filterRow->addWidget(new QLabel(QString::fromLocal8Bit("筛选："), panel_));
    filterRow->addWidget(infoCb_);
    filterRow->addWidget(warnCb_);
    filterRow->addWidget(errorCb_);
    filterRow->addStretch(1);
    filterRow->addWidget(clearBtn_);
    panelLayout->addLayout(filterRow);

    table_ = new QTableWidget(0, 7, panel_);
    table_->setHorizontalHeaderLabels(QStringList()
            << QString::fromLocal8Bit("级别")
            << QString::fromLocal8Bit("时间")
            << QString::fromLocal8Bit("来源")
            << QString::fromLocal8Bit("摘要")
            << QString::fromLocal8Bit("详情/错误码")
            << QString::fromLocal8Bit("Seq")
            << QString::fromLocal8Bit("状态"));
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setMaximumHeight(260);
    table_->verticalHeader()->setVisible(false);
    panelLayout->addWidget(table_);

    panel_->setVisible(false);
    rootLayout->addWidget(panel_);

    connect(expandBtn_, &QPushButton::clicked, this, &AlarmBarWidget::toggleExpanded);
    connect(clearBtn_, &QPushButton::clicked, model_, &AlarmModel::clear);
    const auto refilter = [this] { rebuildTable(); };
    connect(infoCb_, &QCheckBox::toggled, this, refilter);
    connect(warnCb_, &QCheckBox::toggled, this, refilter);
    connect(errorCb_, &QCheckBox::toggled, this, refilter);

    refresh();
}

bool AlarmBarWidget::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == summaryLabel_) && (event->type() == QEvent::MouseButtonRelease)) {
        toggleExpanded();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

quint8 AlarmBarWidget::filterMask() const
{
    quint8 mask = 0U;
    if (infoCb_->isChecked()) {
        mask |= static_cast<quint8>(1U << static_cast<int>(AlarmLevel::Info));
    }
    if (warnCb_->isChecked()) {
        mask |= static_cast<quint8>(1U << static_cast<int>(AlarmLevel::Warning));
    }
    if (errorCb_->isChecked()) {
        mask |= static_cast<quint8>(1U << static_cast<int>(AlarmLevel::Error));
    }
    return mask;
}

void AlarmBarWidget::toggleExpanded()
{
    // 展开/收起只压缩窗口内部内容区：禁止 resize/adjustSize/setGeometry 等
    // 顶层窗口调用（窗口尺寸与最大化状态不变红线）；面板限高由 [alarms]
    // panel_max_height 控制，布局在客户区内自适应
    expanded_ = !expanded_;
    panel_->setVisible(expanded_);
    panel_->setMaximumHeight(AppConfig::instance().alarmPanelMaxHeight());
    expandBtn_->setText(expanded_ ? QString::fromLocal8Bit("收起")
                                  : QString::fromLocal8Bit("展开"));
    if (expanded_) {
        rebuildTable();
    }
}

void AlarmBarWidget::refresh()
{
    AlarmItem top;
    if (!model_->latestSummary(top)) {
        summaryLabel_->setText(QString::fromLocal8Bit("告警：无"));
        summaryLabel_->setStyleSheet(QString::fromLocal8Bit("color:#3b82f6;"));
    } else {
        const QString count = (top.mergeCount > 1)
                ? QStringLiteral(" x%1").arg(top.mergeCount) : QString();
        const QString recovered = top.resolved
                ? QString::fromLocal8Bit("（已恢复）") : QString();
        summaryLabel_->setText(QString::fromLocal8Bit("%1%2%3｜%4 %5")
                .arg(levelText(top.level), count, recovered, top.source, top.summary));
        summaryLabel_->setStyleSheet(
                QStringLiteral("color:%1;").arg(QLatin1String(levelColor(top.level))));
    }
    if (expanded_) {
        rebuildTable();
    }
}

void AlarmBarWidget::rebuildTable()
{
    const QVector<AlarmItem> items = model_->items(filterMask());
    table_->setRowCount(static_cast<int>(items.size()));
    int row = 0;
    for (int i = items.size() - 1; i >= 0; --i) { // 最新在上
        const AlarmItem& item = items.at(i);
        auto* levelItem = new QTableWidgetItem(levelText(item.level));
        levelItem->setForeground(QColor(QLatin1String(levelColor(item.level))));
        table_->setItem(row, 0, levelItem);
        table_->setItem(row, 1, new QTableWidgetItem(
                QDateTime::fromMSecsSinceEpoch(item.lastTimeMs).toString(
                        QStringLiteral("MM-dd HH:mm:ss"))));
        table_->setItem(row, 2, new QTableWidgetItem(item.source));
        table_->setItem(row, 3, new QTableWidgetItem(item.summary));
        QString detail = item.detail;
        if (item.sourceTimeMs >= 0) {
            detail += QStringLiteral("\n") + QString::fromLocal8Bit("对端时间戳: ")
                    + QString::number(item.sourceTimeMs) + QStringLiteral("ms");
        }
        table_->setItem(row, 4, new QTableWidgetItem(detail));
        table_->setItem(row, 5, new QTableWidgetItem(
                (item.seq != 0U) ? QString::number(item.seq) : QStringLiteral("-")));
        QString state;
        if (item.resolved) {
            state = QString::fromLocal8Bit("已恢复 ")
                    + QDateTime::fromMSecsSinceEpoch(item.recoveredAtMs).toString(
                              QStringLiteral("HH:mm:ss"));
        } else {
            state = QString::fromLocal8Bit("活动");
        }
        if (item.mergeCount > 1) {
            state += QStringLiteral(" x%1").arg(item.mergeCount);
        }
        table_->setItem(row, 6, new QTableWidgetItem(state));
        ++row;
    }
    table_->scrollToTop();
}

} // namespace salacia
