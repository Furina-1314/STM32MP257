#include "SshClient.h"

#include <QThread>
#include <QTimer>

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include <libssh/libssh.h>

#include "core/Logger.h"

namespace salacia {

namespace {
constexpr int kCommandTickMs = 20;       // 命令节拍（延迟红线 ≤50ms）
constexpr int kCommandTimeoutMs = 10000; // 单命令超时
constexpr std::size_t kMaxOutputBytes = 64U * 1024U;
constexpr std::size_t kMaxQueueDepth = 64U;
constexpr int kConnectTimeoutSec = 3; // 内网链路：快速失败以支撑紧密重连
} // namespace

SshClient::SshClient(QObject* parent)
    : QObject(parent)
{
}

SshClient::~SshClient()
{
    stop(); // RAII 兜底（幂等）
}

void SshClient::start(const Settings& settings)
{
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    settings_ = settings;

    worker_ = std::make_unique<QThread>();
    worker_->setObjectName(QStringLiteral("salacia-ssh"));
    moveToThread(worker_.get());
    connect(worker_.get(), &QThread::started, this, &SshClient::initOnWorker);
    worker_->start();
    Logger::info(QString::fromLocal8Bit("SSH：工作线程已启动，目标 %1:%2@%3")
                     .arg(settings_.host)
                     .arg(settings_.port)
                     .arg(settings_.user));
}

void SshClient::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // 工作线程自终结（断开会话 -> 停定时器 -> 退出事件循环），限时阶梯
    QMetaObject::invokeMethod(this, &SshClient::shutdownOnWorker,
                              Qt::QueuedConnection);
    if (worker_ != nullptr) {
        if (!worker_->wait(3000)) {
            Logger::error(QString::fromLocal8Bit("SSH：停止超时，请求线程中断"));
            worker_->requestInterruption();
            if (!worker_->wait(2000)) {
                Logger::error(QString::fromLocal8Bit("SSH：线程未响应中断，强制终止"));
                worker_->terminate();
                worker_->wait(1000);
            }
        }
    }
    Logger::info(QString::fromLocal8Bit("SSH：已停止"));
}

void SshClient::initOnWorker()
{
    ssh_init(); // libssh 全局初始化（0.12 必须，否则连接报 Library not initialized）

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setInterval(settings_.reconnectSec * 1000);
    connect(reconnectTimer_, &QTimer::timeout, this, &SshClient::tryConnect);
    reconnectTimer_->start();

    commandTimer_ = new QTimer(this);
    commandTimer_->setInterval(kCommandTickMs);
    connect(commandTimer_, &QTimer::timeout, this, &SshClient::processCommands);
    // commandTimer_ 在连接成功后启动

    tryConnect(); // 立即首连
}

void SshClient::shutdownOnWorker()
{
    if (commandTimer_ != nullptr) {
        commandTimer_->stop();
    }
    if (reconnectTimer_ != nullptr) {
        reconnectTimer_->stop();
    }
    disconnectNow();
    ssh_finalize(); // 与 ssh_init 配对的全局清理
    thread()->quit(); // 自终结事件循环
}

void SshClient::tryConnect()
{
    if (connected_.load(std::memory_order_acquire)) {
        return;
    }
    if (connectOnce()) {
        connected_.store(true, std::memory_order_release);
        {
            // 连接建立瞬间丢弃离线期间积压的陈旧指令（控制新鲜度红线）
            const std::lock_guard<std::mutex> lock(queueMutex_);
            commandQueue_.clear();
        }
        Logger::info(QString::fromLocal8Bit("SSH：已连接 %1:%2")
                         .arg(settings_.host)
                         .arg(settings_.port));
        emit connectionStateChanged(true);
        if (commandTimer_ != nullptr) {
            commandTimer_->start();
        }
    }
    // 失败：保持静默节流（每个重连周期一条错误已由 connectOnce 记录）
}

bool SshClient::connectOnce()
{
    session_ = ssh_new();
    if (session_ == nullptr) {
        emit clientError(QString::fromLocal8Bit("SSH：会话创建失败"));
        return false;
    }

    const QByteArray host = settings_.host.toUtf8();
    const QByteArray user = settings_.user.toUtf8();
    long port = static_cast<long>(settings_.port);
    int timeout = kConnectTimeoutSec;
    int strictHostKey = 0; // 嵌入式自签场景：自动接受新主机密钥（日志提示）

    ssh_options_set(session_, SSH_OPTIONS_HOST, host.constData());
    ssh_options_set(session_, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session_, SSH_OPTIONS_USER, user.constData());
    ssh_options_set(session_, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(session_, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strictHostKey);
    if (!settings_.keyPath.isEmpty()) {
        const QByteArray key = settings_.keyPath.toUtf8();
        ssh_options_set(session_, SSH_OPTIONS_ADD_IDENTITY, key.constData());
    }

    if (ssh_connect(session_) != SSH_OK) {
        emit clientError(QString::fromLocal8Bit("SSH：连接 %1:%2 失败：%3")
                             .arg(settings_.host)
                             .arg(settings_.port)
                             .arg(QString::fromUtf8(ssh_get_error(session_))));
        ssh_free(session_);
        session_ = nullptr;
        return false;
    }

    // 认证：公钥优先（配置了私钥时），失败回退密码
    int rc = SSH_AUTH_DENIED;
    if (!settings_.keyPath.isEmpty()) {
        rc = ssh_userauth_publickey_auto(session_, nullptr, nullptr);
    }
    if ((rc != SSH_AUTH_SUCCESS) && !settings_.password.isEmpty()) {
        const QByteArray pwd = settings_.password.toUtf8();
        rc = ssh_userauth_password(session_, nullptr, pwd.constData());
    }
    if (rc != SSH_AUTH_SUCCESS) {
        emit clientError(QString::fromLocal8Bit("SSH：认证失败（%1@%2）：%3")
                             .arg(settings_.user)
                             .arg(settings_.host)
                             .arg(QString::fromUtf8(ssh_get_error(session_))));
        ssh_disconnect(session_);
        ssh_free(session_);
        session_ = nullptr;
        return false;
    }
    return true;
}

void SshClient::disconnectNow()
{
    if (session_ != nullptr) {
        ssh_disconnect(session_);
        ssh_free(session_);
        session_ = nullptr;
    }
    if (connected_.exchange(false, std::memory_order_acq_rel)) {
        emit connectionStateChanged(false);
    }
}

void SshClient::requestCommand(const QString& command)
{
    const std::lock_guard<std::mutex> lock(queueMutex_);
    if (commandQueue_.size() >= kMaxQueueDepth) {
        commandQueue_.pop_front(); // 超限丢弃最旧（控制命令保新鲜）
    }
    commandQueue_.push_back(command);
}

void SshClient::processCommands()
{
    if (!connected_.load(std::memory_order_acquire) || (session_ == nullptr)) {
        return;
    }

    QString command;
    {
        const std::lock_guard<std::mutex> lock(queueMutex_);
        if (commandQueue_.empty()) {
            return;
        }
        command = commandQueue_.front();
        commandQueue_.pop_front();
    }

    const QByteArray cmdUtf8 = command.toUtf8();
    ssh_channel channel = ssh_channel_new(session_);
    if (channel == nullptr) {
        handleChannelFailure(command);
        return;
    }
    if ((ssh_channel_open_session(channel) != SSH_OK)
        || (ssh_channel_request_exec(channel, cmdUtf8.constData()) != SSH_OK)) {
        emit clientError(QString::fromLocal8Bit("SSH：命令发送失败（%1）：%2")
                             .arg(command)
                             .arg(QString::fromUtf8(ssh_get_error(session_))));
        ssh_channel_free(channel);
        handleChannelFailure(command);
        return;
    }

    // 非阻塞读循环：单命令 10s 超时，输出上限 64KB（网络输入边界红线）
    QByteArray output;
    const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(kCommandTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        char buffer[4096];
        const int n = ssh_channel_read_nonblocking(channel, buffer, sizeof(buffer), 0);
        if (n < 0) {
            break; // 通道错误：终止读取
        }
        if (n > 0) {
            if (output.size() + n > static_cast<int>(kMaxOutputBytes)) {
                output.append(buffer, static_cast<int>(kMaxOutputBytes) - output.size());
            } else {
                output.append(buffer, n);
            }
        }
        if (ssh_channel_is_eof(channel) != 0) {
            break;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    const int exitCode = ssh_channel_get_exit_status(channel);
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);

    emit commandFinished(command, exitCode, QString::fromUtf8(output));
}

void SshClient::handleChannelFailure(const QString& command)
{
    // 通道故障按断线处理：触发重连周期接管，丢弃的命令记录告警
    Logger::warning(QString::fromLocal8Bit("SSH：命令 '%1' 因链路故障丢弃，进入重连")
                        .arg(command));
    disconnectNow();
    if (commandTimer_ != nullptr) {
        commandTimer_->stop();
    }
}

} // namespace salacia
