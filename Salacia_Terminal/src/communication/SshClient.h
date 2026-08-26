#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

struct ssh_session_struct; // libssh C 句柄前置声明（第三方 C 库边界）

class QThread;
class QTimer;

namespace salacia {

// SSH 客户端（libssh 封装，Worker-Object）
//
// 线程模型（多线程规范红线）：
//  - 全部阻塞 I/O（连接/认证/命令执行）在专属工作线程事件循环内完成，
//    GUI 线程仅 start/stop/requestCommand（非阻塞投递）；
//  - 退出：工作线程自终结 + 主线程限时阶梯等待（与其他 Worker 一致）。
//
// 行为：
//  - 认证：私钥优先（ssh_key_path 配置时），失败回退密码；
//    主机密钥策略为自动接受新主机（嵌入式板端自签场景，日志提示）；
//  - 断线自动重连：ssh_reconnect_sec 周期，重连期间命令排队；
//  - 命令通道：FIFO 队列（上限 64，超限丢弃最旧），工作线程 20ms 节拍
//    执行（指令响应延迟 ≤50ms 红线），单命令 10s 超时，输出上限 64KB。
class SshClient : public QObject
{
    Q_OBJECT

public:
    struct Settings
    {
        QString host;
        quint16 port = 22;
        QString user;
        QString password;
        QString keyPath;       // 可选：私钥路径（公钥认证优先）
        int reconnectSec = 5;  // 断线重连周期（秒）
    };

    explicit SshClient(QObject* parent = nullptr); // Worker：禁止设父对象
    ~SshClient() override;
    Q_DISABLE_COPY(SshClient)

    // 主线程调用：启动工作线程并开始自动连接
    void start(const Settings& settings);
    // 幂等停止：断开 -> 工作线程自终结 -> 主线程限时收割
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    bool isConnected() const { return connected_.load(std::memory_order_acquire); }

    // 任意线程调用：命令入队（工作线程串行执行）
    void requestCommand(const QString& command);

signals:
    // 均自工作线程经排队连接发出
    void connectionStateChanged(bool connected);
    void commandFinished(const QString& command, int exitCode, const QString& output);
    void clientError(const QString& message);

private slots:
    void initOnWorker();
    void shutdownOnWorker();
    void tryConnect();      // 重连节拍：未连接时尝试建立会话
    void processCommands(); // 命令节拍：连接就绪时逐条执行

private:
    bool connectOnce();
    void disconnectNow(); // 工作线程内释放会话
    void handleChannelFailure(const QString& command); // 通道故障->断线重连

    Settings settings_;

    std::unique_ptr<QThread> worker_;
    QTimer* reconnectTimer_ = nullptr; // 工作线程内创建/销毁
    QTimer* commandTimer_ = nullptr;

    ssh_session_struct* session_ = nullptr; // libssh C 句柄（工作线程独占）

    std::mutex queueMutex_;             // 短临界区保护命令队列
    std::deque<QString> commandQueue_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
};

} // namespace salacia
