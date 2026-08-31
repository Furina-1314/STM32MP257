// AlarmModel 单元测试：级别/合并窗口/容量/Error 不被覆盖/筛选
#include <QtTest/qtest.h>

#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include "core/AlarmModel.h"
#include "core/AppConfig.h"

using namespace salacia;

namespace {

qint64 g_fakeNow = 1000;
qint64 fakeClock() { return g_fakeNow; }

QString loadIniPath()
{
    static QTemporaryDir dir;
    return dir.filePath(QStringLiteral("alarms.ini"));
}

bool loadIni(int maxItems, int mergeMs)
{
    const QString path = loadIniPath();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(QStringLiteral("[alarms]\nmax_items = %1\nmerge_window_ms = %2\nlog_alarms = false\n")
                       .arg(maxItems)
                       .arg(mergeMs)
                       .toUtf8());
    file.close();
    return AppConfig::instance().load(path);
}

} // namespace

class TestAlarmModel : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(loadIni(200, 5000)); }

    void levelsAndLatest()
    {
        AlarmModel model;
        model.setClockForTest(&fakeClock);
        g_fakeNow = 1000;
        model.add(AlarmLevel::Info, QStringLiteral("ui"), QStringLiteral("i1"));
        g_fakeNow = 2000;
        model.add(AlarmLevel::Warning, QStringLiteral("net"), QStringLiteral("w1"));
        AlarmItem top;
        QVERIFY(model.latestSummary(top));
        QCOMPARE(top.summary, QStringLiteral("w1")); // Warning 压过较早 Info
        g_fakeNow = 3000;
        model.add(AlarmLevel::Info, QStringLiteral("ui"), QStringLiteral("i2"));
        QVERIFY(model.latestSummary(top));
        QCOMPARE(top.summary, QStringLiteral("w1")); // 新 Info 不覆盖 Warning
    }

    void errorNotCovered()
    {
        AlarmModel model;
        model.setClockForTest(&fakeClock);
        g_fakeNow = 1000;
        model.add(AlarmLevel::Error, QStringLiteral("tcp"), QStringLiteral("E"));
        g_fakeNow = 9000;
        model.add(AlarmLevel::Info, QStringLiteral("ui"), QStringLiteral("I"));
        g_fakeNow = 9500;
        model.add(AlarmLevel::Warning, QStringLiteral("net"), QStringLiteral("W"));
        AlarmItem top;
        QVERIFY(model.latestSummary(top));
        QCOMPARE(top.level, AlarmLevel::Error); // Error 持续置顶
        QCOMPARE(top.summary, QStringLiteral("E"));
    }

    void mergeWindowCounts()
    {
        QVERIFY(loadIni(200, 5000));
        AlarmModel model;
        model.setClockForTest(&fakeClock);
        g_fakeNow = 1000;
        model.add(AlarmLevel::Warning, QStringLiteral("net"), QStringLiteral("dup"));
        g_fakeNow = 2000;
        model.add(AlarmLevel::Warning, QStringLiteral("net"), QStringLiteral("dup"));
        g_fakeNow = 4000;
        model.add(AlarmLevel::Warning, QStringLiteral("net"), QStringLiteral("dup"));
        QCOMPARE(model.count(), 1); // 窗口内合并
        const QVector<AlarmItem> items = model.items();
        QCOMPARE(items.first().mergeCount, 3);
        QCOMPARE(items.first().firstTimeMs, qint64(1000)); // 保留首条时间
        QCOMPARE(items.first().lastTimeMs, qint64(4000));  // 刷新最近时间

        g_fakeNow = 12000; // 超出 5000ms 窗口 -> 新条目
        model.add(AlarmLevel::Warning, QStringLiteral("net"), QStringLiteral("dup"));
        QCOMPARE(model.count(), 2);

        // 不同 source 同 summary 不合并
        g_fakeNow = 12001;
        model.add(AlarmLevel::Warning, QStringLiteral("video"), QStringLiteral("dup"));
        QCOMPARE(model.count(), 3);
    }

    void capacityDropsOldestLowLevelFirst()
    {
        QVERIFY(loadIni(12, 0)); // 容量 12（boundedInt 下限 10），禁合并
        QCOMPARE(AppConfig::instance().alarmMaxItems(), 12);
        AlarmModel model;
        model.setClockForTest(&fakeClock);
        // 1 条 Error + 9 条 Info + 3 条 Warning = 13 条 -> 触发淘汰
        g_fakeNow = 1000;
        model.add(AlarmLevel::Error, QStringLiteral("s"), QStringLiteral("E1"));
        for (int i = 1; i <= 9; ++i) {
            g_fakeNow = 1000 + i;
            model.add(AlarmLevel::Info, QStringLiteral("s"),
                      QStringLiteral("I%1").arg(i));
        }
        for (int i = 1; i <= 3; ++i) {
            g_fakeNow = 2000 + i;
            model.add(AlarmLevel::Warning, QStringLiteral("s"),
                      QStringLiteral("W%1").arg(i));
        }
        QCOMPARE(model.count(), 12); // 淘汰 1 条
        const QVector<AlarmItem> items = model.items();
        // 应丢最旧低级别（I1 = 最早的 Info），保留 E1 与全部 Warning
        bool hasI1 = false;
        bool hasI2 = false;
        bool hasE1 = false;
        int warnCount = 0;
        for (const AlarmItem& item : items) {
            hasI1 = hasI1 || (item.summary == QStringLiteral("I1"));
            hasI2 = hasI2 || (item.summary == QStringLiteral("I2"));
            hasE1 = hasE1 || (item.summary == QStringLiteral("E1"));
            if (item.level == AlarmLevel::Warning) {
                ++warnCount;
            }
        }
        QVERIFY(!hasI1);
        QVERIFY(hasI2 && hasE1);
        QCOMPARE(warnCount, 3);
    }

    void filterByLevel()
    {
        AlarmModel model;
        model.setClockForTest(&fakeClock);
        g_fakeNow = 1000;
        model.add(AlarmLevel::Info, QStringLiteral("a"), QStringLiteral("i"));
        model.add(AlarmLevel::Error, QStringLiteral("b"), QStringLiteral("e"));
        model.add(AlarmLevel::Warning, QStringLiteral("c"), QStringLiteral("w"));
        const quint8 warnErr = (1U << static_cast<int>(AlarmLevel::Warning))
                | (1U << static_cast<int>(AlarmLevel::Error));
        QCOMPARE(model.items(warnErr).size(), 2);
        QCOMPARE(model.items(0U).size(), 3); // 0 = 全部
    }

    void detailAndSeqKept()
    {
        AlarmModel model;
        model.setClockForTest(&fakeClock);
        g_fakeNow = 1000;
        model.add(AlarmLevel::Error, QStringLiteral("tcp"), QStringLiteral("bad"),
                  QStringLiteral("err=7"), 42U);
        const AlarmItem item = model.items().first();
        QCOMPARE(item.detail, QStringLiteral("err=7"));
        QCOMPARE(item.seq, quint16(42));
    }
};

QTEST_APPLESS_MAIN(TestAlarmModel)
#include "test_alarmmodel.moc"
