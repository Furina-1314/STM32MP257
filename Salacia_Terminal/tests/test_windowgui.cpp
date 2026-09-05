// 窗口尺寸 GUI 测试：告警栏展开/收起与导航折叠不得改变顶层窗口尺寸/最大化状态
// （二轮提示词 §十一：普通窗口前后 geometry 不变；最大化恒 isMaximized；无尺寸漂移）
#include <QtTest/qtest.h>

#include <QFile>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QWidget>

#include "core/AlarmModel.h"
#include "core/AppConfig.h"
#include "core/Logger.h"
#include "exwinuinavigationview.h"
#include "widgets/AlarmBarWidget.h"

using namespace salacia;

namespace {

bool loadTestIni()
{
    static QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("windowgui.ini"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(QByteArrayLiteral(
            "[alarms]\nmax_items = 100\nmerge_window_ms = 1000\nlog_alarms = false\n"
            "panel_max_height = 200\n"));
    file.close();
    return AppConfig::instance().load(path);
}

// 最小宿主窗口：复刻 MainWindow 结构（导航列 + 内容列[告警栏 + 页栈]）
struct HostWindow
{
    QWidget window;
    AlarmBarWidget* alarmBar = nullptr;
    ExWinUINavigationView* nav = nullptr;

    HostWindow()
    {
        auto* centralLayout = new QHBoxLayout(&window);
        centralLayout->setContentsMargins(0, 0, 0, 0);
        centralLayout->setSpacing(0);

        auto* navColumn = new QWidget(&window);
        auto* navLayout = new QVBoxLayout(navColumn);
        navLayout->setContentsMargins(0, 4, 0, 0);
        navLayout->setSpacing(0);
        nav = new ExWinUINavigationView(navColumn);
        navLayout->addWidget(nav);
        centralLayout->addWidget(navColumn);

        auto* content = new QWidget(&window);
        auto* contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(4, 4, 4, 4);
        alarmBar = new AlarmBarWidget(&model, content);
        contentLayout->addWidget(alarmBar);
        auto* stack = new QStackedWidget(content);
        for (int i = 0; i < 3; ++i) {
            stack->addWidget(new QWidget(stack));
        }
        contentLayout->addWidget(stack, 1);
        centralLayout->addWidget(content, 1);

        nav->addMainNavigationItem(QStringLiteral("P1"), 0);
        nav->addMainNavigationItem(QStringLiteral("P2"), 1);
        nav->setStackedWidget(stack);
        nav->setNavigationExpanded(true, false);
        nav->setSelectedPageIndex(0);
    }

private:
    AlarmModel model{nullptr};
};

} // namespace

class TestWindowGui : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(loadTestIni()); }

    void alarmToggleKeepsNormalSize();
    void alarmToggleKeepsMaximized();
    void navCollapseKeepsNormalSize();
    void navCollapseKeepsMaximized();
    void repeatedTogglesNoDrift();
};

void TestWindowGui::alarmToggleKeepsNormalSize()
{
    HostWindow host;
    host.window.resize(1000, 700);
    host.window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host.window));
    QTest::qWait(200); // 布局稳定

    const QSize sizeBefore = host.window.size();
    for (int i = 0; i < 3; ++i) {
        host.alarmBar->toggleExpanded(); // 展开
        QTest::qWait(200);
        QCOMPARE(host.window.size(), sizeBefore); // 尺寸不变（仅内部压缩）
        host.alarmBar->toggleExpanded(); // 收起
        QTest::qWait(200);
        QCOMPARE(host.window.size(), sizeBefore);
    }
}

void TestWindowGui::alarmToggleKeepsMaximized()
{
    HostWindow host;
    host.window.resize(1000, 700);
    host.window.showMaximized();
    QVERIFY(QTest::qWaitForWindowExposed(&host.window));
    QTest::qWait(200);
    QVERIFY(host.window.isMaximized());

    for (int i = 0; i < 3; ++i) {
        host.alarmBar->toggleExpanded();
        QTest::qWait(200);
        QVERIFY2(host.window.isMaximized(), "maximized state must survive expand");
        host.alarmBar->toggleExpanded();
        QTest::qWait(200);
        QVERIFY2(host.window.isMaximized(), "maximized state must survive collapse");
    }
    host.window.showNormal();
    QTest::qWait(150);
}

void TestWindowGui::navCollapseKeepsNormalSize()
{
    HostWindow host;
    host.window.resize(1000, 700);
    host.window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host.window));
    QTest::qWait(200);

    const QSize sizeBefore = host.window.size();
    for (int i = 0; i < 3; ++i) {
        host.nav->setNavigationExpanded(false); // 折叠（内部动画完成后布局稳定）
        QTest::qWait(400);
        QCOMPARE(host.window.size(), sizeBefore);
        host.nav->setNavigationExpanded(true);
        QTest::qWait(400);
        QCOMPARE(host.window.size(), sizeBefore);
    }
}

void TestWindowGui::navCollapseKeepsMaximized()
{
    HostWindow host;
    host.window.resize(1000, 700);
    host.window.showMaximized();
    QVERIFY(QTest::qWaitForWindowExposed(&host.window));
    QTest::qWait(200);
    QVERIFY(host.window.isMaximized());

    for (int i = 0; i < 3; ++i) {
        host.nav->setNavigationExpanded(false);
        QTest::qWait(400);
        QVERIFY2(host.window.isMaximized(), "maximized state must survive collapse");
        host.nav->setNavigationExpanded(true);
        QTest::qWait(400);
        QVERIFY2(host.window.isMaximized(), "maximized state must survive expand");
    }
    host.window.showNormal();
    QTest::qWait(150);
}

void TestWindowGui::repeatedTogglesNoDrift()
{
    HostWindow host;
    host.window.resize(1024, 720);
    host.window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host.window));
    QTest::qWait(200);

    const QSize sizeBefore = host.window.size();
    bool alarmExpanded = false;
    for (int i = 0; i < 10; ++i) {
        host.alarmBar->toggleExpanded();
        alarmExpanded = !alarmExpanded;
        host.nav->setNavigationExpanded((i % 2) == 0);
        QTest::qWait(250);
        QCOMPARE(host.window.size(), sizeBefore); // 重复操作无尺寸漂移
    }
    if (alarmExpanded) {
        host.alarmBar->toggleExpanded(); // 复位
    }
}

QTEST_MAIN(TestWindowGui)
#include "test_windowgui.moc"
