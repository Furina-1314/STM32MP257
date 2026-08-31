#include "AlarmModel.h"

#include <QDateTime>

#include <algorithm>

#include "core/AppConfig.h"
#include "core/Logger.h"

namespace salacia {

namespace {
qint64 defaultClock()
{
    return QDateTime::currentMSecsSinceEpoch();
}
} // namespace

AlarmModel::AlarmModel(QObject* parent)
    : QObject(parent)
{
    logEnabled_ = AppConfig::instance().alarmLogEnabled();
    clock_ = &defaultClock;
}

void AlarmModel::setClockForTest(qint64 (*clock)())
{
    clock_ = (clock != nullptr) ? clock : &defaultClock;
}

void AlarmModel::add(AlarmLevel level, const QString& source, const QString& summary,
                     const QString& detail, quint16 seq, qint64 sourceTimeMs)
{
    // 容量/合并窗口实时读配置（设置页运行期可调）
    const int maxItems = AppConfig::instance().alarmMaxItems();
    const int mergeWindowMs = AppConfig::instance().alarmMergeWindowMs();
    bool merged = false;
    {
        const std::lock_guard<QMutex> lock(mutex_);
        const qint64 now = clock_();

        // 鍚岄敭绐楀彛鍐呭悎骞讹細璁℃暟 + 鍒锋柊鏈?杩戞椂闂?
        for (int i = items_.size() - 1; i >= 0; --i) {
            AlarmItem& item = items_[i];
            if ((item.source == source) && (item.summary == summary)
                && ((now - item.lastTimeMs) <= mergeWindowMs)) {
                ++item.mergeCount;
                item.lastTimeMs = now;
                if (level > item.level) {
                    item.level = level; // 合并期间升级保留最高级
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            AlarmItem item;
            item.level = level;
            item.firstTimeMs = now;
            item.lastTimeMs = now;
            item.source = source;
            item.summary = summary;
            item.detail = detail;
            item.seq = seq;
            item.sourceTimeMs = sourceTimeMs;
            items_.append(item);
        }
        trimLocked(maxItems);
    }

    // 棣栨潯钀芥棩蹇楋紙鍚堝苟璁℃暟涓嶆墦鏃ュ織锛岄伩鍏嶉?庢毚锛?
    if (logEnabled_ && !merged) {
        const QString line = QString::fromLocal8Bit("鍛婅??[%1] %2: %3 %4")
                .arg(level == AlarmLevel::Error ? QString::fromLocal8Bit("閿欒??")
                      : level == AlarmLevel::Warning ? QString::fromLocal8Bit("璀﹀憡")
                                                     : QString::fromLocal8Bit("淇℃伅"))
                .arg(source, summary, detail);
        if (level == AlarmLevel::Error) {
            Logger::error(line);
        } else if (level == AlarmLevel::Warning) {
            Logger::warning(line);
        } else {
            Logger::info(line);
        }
    }
    emit alarmsChanged();
}

bool AlarmModel::latestSummary(AlarmItem& out) const
{
    const std::lock_guard<QMutex> lock(mutex_);
    int best = -1;
    for (int i = 0; i < items_.size(); ++i) {
        if (best < 0) {
            best = i;
            continue;
        }
        const AlarmItem& candidate = items_.at(i);
        const AlarmItem& current = items_.at(best);
        // 楂樼骇鍒?浼樺厛锛涘悓绾у彇鏈?杩?
        if ((candidate.level > current.level)
            || ((candidate.level == current.level)
                && (candidate.lastTimeMs >= current.lastTimeMs))) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    out = items_.at(best);
    return true;
}

QVector<AlarmItem> AlarmModel::items(quint8 mask) const
{
    const std::lock_guard<QMutex> lock(mutex_);
    if (mask == 0U) {
        return items_;
    }
    QVector<AlarmItem> filtered;
    for (const AlarmItem& item : items_) {
        const quint8 bit = static_cast<quint8>(1U << static_cast<int>(item.level));
        if ((mask & bit) != 0U) {
            filtered.append(item);
        }
    }
    return filtered;
}

int AlarmModel::count() const
{
    const std::lock_guard<QMutex> lock(mutex_);
    return items_.size();
}

void AlarmModel::clear()
{
    {
        const std::lock_guard<QMutex> lock(mutex_);
        items_.clear();
    }
    emit alarmsChanged();
}

void AlarmModel::markRecovered(const QString& source, const QString& summary)
{
    {
        const std::lock_guard<QMutex> lock(mutex_);
        const qint64 now = clock_();
        for (int i = items_.size() - 1; i >= 0; --i) {
            AlarmItem& item = items_[i];
            if ((item.source == source) && (item.summary == summary)
                && !item.resolved) {
                item.resolved = true;
                item.recoveredAtMs = now;
                break;
            }
        }
    }
    emit alarmsChanged();
}

void AlarmModel::trimLocked(int maxItems)
{
    while (static_cast<int>(items_.size()) > maxItems) {
        // 鍏堜涪鏈?鏃х殑浣庣骇鍒?鏉＄洰锛涘叏鍚岀骇鍒欎涪鏈?鏃?
        int drop = 0;
        for (int i = 1; i < items_.size(); ++i) {
            if (items_.at(i).level < items_.at(drop).level) {
                drop = i;
            }
        }
        items_.remove(drop);
    }
}

} // namespace salacia
