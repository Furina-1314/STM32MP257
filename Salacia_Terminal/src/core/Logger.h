#pragma once

#include <QFile>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace salacia {

// 后台异步日志器（Worker-Object 模式）
//
// 线程模型（遵循多线程规范）：
//  - Logger 实例经 moveToThread() 移入独立日志线程，文件 I/O 全部在该线程
//    事件循环内完成；GUI / GStreamer / 推理等线程仅做无阻塞入队
//    （短临界区互斥 + drainPending_ 合并投递，杜绝事件洪泛）。
//  - UI 订阅 logReady() 时必须显式使用 Qt::QueuedConnection
//    （信号自日志线程发出，跨线程 UI 更新红线）。
//  - 安全退出：Logger::shutdown() 先冲刷队列（BlockingQueuedConnection），
//    再 quit()/wait() 日志线程，最后关闭文件；析构函数 RAII 兜底。
//  - 日志为低频数据，允许 std::mutex；高频视频帧才强制无锁 RingBuffer。
class Logger : public QObject
{
    Q_OBJECT

public:
    enum class Level
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3
    };

    // 主线程调用一次：创建日志线程、打开日志文件、接管 Qt 消息输出
    static void init(const QString& logDir);

    // 幂等安全退出：冲刷队列 -> 停止日志线程 -> 关闭文件
    static void shutdown();

    // 任意线程可调用的线程安全日志入口
    static void log(Level level, const QString& message);

    static void debug(const QString& message) { log(Level::Debug, message); }
    static void info(const QString& message) { log(Level::Info, message); }
    static void warning(const QString& message) { log(Level::Warning, message); }
    static void error(const QString& message) { log(Level::Error, message); }

    static bool isInitialized() { return initialized_.load(); }

signals:
    // 每条日志落盘时广播（level 为 int 以便跨线程排队）；
    // UI 侧连接时必须显式指定 Qt::QueuedConnection
    void logReady(int level, const QString& message);

private slots:
    // 仅在日志线程执行：出队 -> 写文件 -> 广播
    void drainQueue();
    void drainAndQuit(); // 冲刷后自终结（退出路径专用）

private:
    explicit Logger(QObject* parent = nullptr);
    ~Logger() override; // RAII 兜底：未显式 shutdown 时在此安全收尾

    static Logger& instance();
    static void qtMessageHandler(QtMsgType type,
                                 const QMessageLogContext& context,
                                 const QString& message);
    static QString levelToString(Level level);

    void openFile(const QString& logDir);
    void rotateIfNeeded();
    void writeLine(Level level, quintptr tid, const QString& message);

    std::unique_ptr<QThread> workerThread_;    // 日志工作线程（标准 QThread + moveToThread）
    QFile file_;                               // 移入线程后由日志线程独占使用
    QString logDir_;                           // 轮转时复用的日志目录

    // 队列项：tid 在入队时打戳（调用线程），落盘时不因异步而失真
    struct LogItem
    {
        int level = 0;
        quintptr tid = 0;
        QString message;
    };

    std::mutex queueMutex_;               // 短临界区保护队列（日志为低频路径）
    std::deque<LogItem> queue_;           // 待写入日志
    std::atomic<bool> drainPending_{false};    // 投递合并标志：挂起中则不再重复投递

    static std::atomic<bool> initialized_;

    static constexpr qint64 kMaxLogBytes = 10LL * 1024LL * 1024LL; // 单文件 10MB 轮转
};

} // namespace salacia
