#include "Logger.h"

#include <QDateTime>
#include <QDir>

#include <cstdio>
#include <cstdlib>

#include "AppConfig.h"

namespace salacia {

std::atomic<bool> Logger::initialized_{false};

namespace {
// 递归保护：Qt 消息处理器 -> Logger -> (可能的) Qt 内部告警
thread_local bool t_inQtMessageHandler = false;
}

Logger::Logger(QObject* parent)
    : QObject(parent)
    , workerThread_(std::make_unique<QThread>())
{
}

Logger::~Logger()
{
    shutdown();
}

Logger& Logger::instance()
{
    static Logger logger; // Meyers 单例：init() 首次调用时于主线程构造
    return logger;
}

void Logger::init(const QString& logDir)
{
    if (initialized_.load()) {
        return; // 禁止重复初始化
    }

    Logger& logger = instance();

    // 轮转阈值来自 [log] max_file_bytes（init 于 AppConfig 加载后调用）
    logger.maxFileBytes_ = AppConfig::instance().logMaxFileBytes();

    // 文件在移入工作线程前打开，此后由日志线程独占使用（无并发访问）
    logger.openFile(logDir);

    logger.workerThread_->setObjectName(QStringLiteral("salacia-logger"));
    logger.moveToThread(logger.workerThread_.get());
    logger.workerThread_->start();

    qInstallMessageHandler(&Logger::qtMessageHandler);

    initialized_.store(true);
    logger.info(QString::fromLocal8Bit("==== Salacia_Terminal 日志系统启动 ===="));
}

void Logger::shutdown()
{
    if (!initialized_.exchange(false)) {
        return;
    }

    qInstallMessageHandler(nullptr);

    Logger& logger = instance();

    // 日志线程内：冲刷队列后自退出（非阻塞投递 + 限时阶梯护栏，杜绝死锁）
    QMetaObject::invokeMethod(&logger, "drainAndQuit", Qt::QueuedConnection);
    const AppConfig& cfg = AppConfig::instance();
    if (!logger.workerThread_->wait(cfg.workerStopWaitMs())) {
        logger.workerThread_->requestInterruption();
        if (!logger.workerThread_->wait(cfg.workerInterruptWaitMs())) {
            logger.workerThread_->terminate();
            logger.workerThread_->wait(cfg.workerTerminateWaitMs());
        }
    }

    if (logger.file_.isOpen()) {
        logger.file_.flush();
        logger.file_.close();
    }
}

void Logger::log(Level level, const QString& message)
{
    if (!initialized_.load()) {
        // 初始化前的日志直接走标准错误（本地 GBK 控制台）
        static_cast<void>(std::fprintf(stderr, "%s\n", message.toLocal8Bit().constData()));
        return;
    }

    Logger& logger = instance();

    {
        const std::lock_guard<std::mutex> lock(logger.queueMutex_);
        LogItem item;
        item.level = static_cast<int>(level);
        item.tid = reinterpret_cast<quintptr>(QThread::currentThreadId());
        item.message = message;
        logger.queue_.push_back(std::move(item));
    }

    // 合并投递：仅当无挂起 drain 时才投递一次事件
    if (!logger.drainPending_.exchange(true)) {
        QMetaObject::invokeMethod(&logger, "drainQueue", Qt::QueuedConnection);
    }
}

void Logger::drainQueue()
{
    drainPending_.store(false);

    for (;;) {
        LogItem item;
        {
            const std::lock_guard<std::mutex> lock(queueMutex_);
            if (queue_.empty()) {
                break;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }

        writeLine(static_cast<Level>(item.level), item.tid, item.message);
        emit logReady(item.level, item.message);
    }
}

void Logger::drainAndQuit()
{
    drainQueue();
    thread()->quit(); // 自终结：主线程限时 wait 收割
}

void Logger::openFile(const QString& logDir)
{
    if (!QDir().mkpath(logDir)) {
        static_cast<void>(std::fprintf(stderr, "Logger: cannot create log dir '%s'\n",
                     logDir.toLocal8Bit().constData()));
        return;
    }
    logDir_ = logDir;

    const QString fileName = QStringLiteral("salacia_%1.log")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

    file_.setFileName(QDir(logDir).filePath(fileName));
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        static_cast<void>(std::fprintf(stderr, "Logger: cannot open log file '%s'\n",
                     file_.fileName().toLocal8Bit().constData()));
    }
}

void Logger::rotateIfNeeded()
{
    if (file_.size() < maxFileBytes_) {
        return;
    }

    file_.close();

    const QString fileName = QStringLiteral("salacia_%1.log")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    file_.setFileName(QDir(logDir_).filePath(fileName)); // 复用原目录
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        static_cast<void>(std::fprintf(stderr, "Logger: cannot rotate log file '%s'\n",
                     file_.fileName().toLocal8Bit().constData()));
    }
}

void Logger::writeLine(Level level, quintptr tid, const QString& message)
{
    rotateIfNeeded();

    const QString line = QStringLiteral("%1 [%2] [tid 0x%3] %4\n")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                 levelToString(level),
                 QString::number(tid, 16),
                 message);

    if (file_.isOpen()) {
        file_.write(line.toUtf8());
        file_.flush(); // 低频日志，逐行冲刷以保证崩溃现场不丢失
    }
}

QString Logger::levelToString(Level level)
{
    switch (level) {
    case Level::Debug:   return QStringLiteral("DEBUG  ");
    case Level::Info:    return QStringLiteral("INFO   ");
    case Level::Warning: return QStringLiteral("WARNING");
    case Level::Error:   return QStringLiteral("ERROR  ");
    default:             return QStringLiteral("UNKNOWN");
    }
}

void Logger::qtMessageHandler(QtMsgType type,
                              const QMessageLogContext& context,
                              const QString& message)
{
    if (t_inQtMessageHandler) {
        return; // 递归保护
    }
    t_inQtMessageHandler = true;

    Level level = Level::Debug;
    switch (type) {
    case QtDebugMsg:    level = Level::Debug;   break;
    case QtInfoMsg:     level = Level::Info;    break;
    case QtWarningMsg:  level = Level::Warning; break;
    case QtCriticalMsg:
    case QtFatalMsg:    level = Level::Error;   break;
    default:            break;
    }

    // __FILE__ 为 GBK 编码窄字符串（/source-charset:GBK），必须 fromLocal8Bit
    const QString location = (context.file != nullptr)
            ? QStringLiteral("%1:%2 ").arg(QString::fromLocal8Bit(context.file)).arg(context.line)
            : QString();

    log(level, location + message);

    t_inQtMessageHandler = false;

    if (type == QtFatalMsg) {
        std::abort(); // Qt 契约：qFatal 处理器必须终止进程
    }
}

} // namespace salacia
