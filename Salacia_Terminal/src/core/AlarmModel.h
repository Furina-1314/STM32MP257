#pragma once

#include <QObject>
#include <QString>

#include <QMutex>

#include <mutex>
#include <QVector>

namespace salacia {

// 鍛婅?︾骇鍒?锛堥《閮ㄦ爮 Error/绱ф?ヤ簨浠朵笉寰楄??鏅?閫? Info 瑕嗙洊绾㈢嚎锛?
enum class AlarmLevel
{
    Info = 0,
    Warning = 1,
    Error = 2,
};

// 鍗曟潯鍛婅?︼紙鍚?閲嶅?嶅悎骞惰?℃暟锛氫繚鐣欓?栨潯鍐呭?? + 鏈?杩戞椂闂? + 鍚堝苟娆℃暟锛?
struct AlarmItem
{
    AlarmLevel level = AlarmLevel::Info;
    qint64 firstTimeMs = 0;      // Windows 系统时间（首条）
    qint64 lastTimeMs = 0;       // Windows 系统时间（最近）
    qint64 sourceTimeMs = -1;    // 对端源时间戳（A35 事件；-1 = 无）
    QString source;   // 来源：网络/解析/交互/视频/配置/日志/A35 等
    QString summary;  // 摘要（顶部栏展示）
    QString detail;   // 技术详情/错误码
    quint16 seq = 0U; // 关联请求序号（0 = 无）
    int mergeCount = 1;
    bool resolved = false;       // 恢复状态（markRecovered 置位）
    qint64 recoveredAtMs = 0;    // 恢复时刻（Windows 系统时间）
};

// 鍛婅?︿腑蹇冩暟鎹?妯″瀷锛堜富绾跨▼锛涘?归噺/鍚堝苟绐楀彛/鏄?鍚﹁惤鏃ュ織鍏ㄩ儴鏉ヨ嚜 [alarms]锛?
//
// 鍚堝苟锛氬悓 source+summary 涓旇窛涓婃?′笉瓒? merge_window_ms -> 璁℃暟+1 骞跺埛鏂?
// lastTimeMs锛堜繚鐣? firstTimeMs 涓庨?栨潯璇︽儏锛夛紱瓒呯獥鎴栦笉鍚岄敭 -> 鏂版潯鐩?銆?
// 瀹归噺锛氳秴杩? max_items 涓㈡渶鏃э紱Error 浼樺厛淇濈暀锛堝悓鐣屽唴鍏堜涪浣庣骇鍒?鏈?鏃ф潯锛夈??
// 椤堕儴鎽樿?侊細鏈?楂樼骇鍒?涓?鐨勬渶杩戜竴鏉★紙Error 鎸佺画缃?椤讹紝涓嶈?? Info 瑕嗙洊锛夈??
class AlarmModel : public QObject
{
    Q_OBJECT

public:
    explicit AlarmModel(QObject* parent = nullptr);

    // 杩藉姞鍛婅?︼紙浠绘剰鏉ユ簮缁熶竴鍏ュ彛锛涘彲浠庝换鎰忕嚎绋嬬粡鎺掗槦淇″彿鍒拌揪锛?
    void add(AlarmLevel level, const QString& source, const QString& summary,
             const QString& detail = QString(), quint16 seq = 0U,
             qint64 sourceTimeMs = -1);
    // 标记恢复（配对的告警恢复事件：最近未恢复的同源同摘要条目）
    void markRecovered(const QString& source, const QString& summary);

    // 椤堕儴鏍忓綋鍓嶆憳瑕侊紙鏈?楂樼骇鍒?涓?鏈?杩戜竴鏉★紱鏃犲憡璀﹁繑鍥? false锛?
    bool latestSummary(AlarmItem& out) const;

    // 鍏ㄩ儴鏉＄洰锛堟椂闂村崌搴忥級锛沵ask 浣嶆帺鐮? = 1<<level 缁勫悎锛?0 = 鍏ㄩ儴锛?
    QVector<AlarmItem> items(quint8 mask = 0U) const;

    int count() const;
    void clear();

    // 娴嬭瘯閽╁瓙锛氱洿鎺ユ寚瀹氬悎骞跺垽瀹氭椂閽?
    void setClockForTest(qint64 (*clock)());

signals:
    void alarmsChanged(); // 鏃犺浇鑽烽?氱煡锛歎I 鏀跺埌鍚庤嚜琛屾媺鍙? items/latestSummary

private:
    void trimLocked(int maxItems);

    bool logEnabled_ = true;

    mutable QMutex mutex_;
    QVector<AlarmItem> items_;

    qint64 (*clock_)() = nullptr; // 榛樿?? QDateTime::currentMSecsSinceEpoch
};

} // namespace salacia
