#include "CommandPageWidget.h"

#include <QComboBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "communication/FunctionRegistry.h"
#include "communication/WireConstants.h"
#include "control/ControlViewModel.h"
#include "core/AppConfig.h"
#include "core/SafetyStateModel.h"
#include "video/VideoFrameHub.h"
#include "widgets/ControlAreaWidget.h"
#include "widgets/SwitchButtonWidget.h"
#include "widgets/VideoGLWidget.h"

namespace salacia {

namespace {
constexpr int kMaxResultRows = 300;

QString funcName(quint16 funcId)
{
    const wire::FunctionEntry* entry = wire::FunctionRegistry::findByFuncId(funcId);
    return (entry != nullptr) ? QString::fromLatin1(entry->name)
                              : QStringLiteral("0x%1").arg(funcId, 4, 16, QLatin1Char('0'));
}
} // namespace

CommandPageWidget::CommandPageWidget(ControlViewModel* viewModel,
                                     SafetyStateModel* safety,
                                     VideoFrameHub* videoHub,
                                     QWidget* parent)
    : QWidget(parent)
    , vm_(viewModel)
    , safety_(safety)
{
    auto* rootLayout = new QVBoxLayout(this);

    auto* notice = new QLabel(QString::fromLocal8Bit(
            "TCP wire 为草案 v1（待 A35 确认）；全部命令仅限已注册函数，"
            "状态以板端 ACK 为准。"), this);
    notice->setWordWrap(true);
    rootLayout->addWidget(notice);

    auto* body = new QHBoxLayout();
    auto* formColumn = new QVBoxLayout();

    // ---- 左上角小尺寸实时视频（共享主页 VideoFrameHub：单管线/单端口/零拷贝）----
    if (videoHub != nullptr) {
        const AppConfig& cfg = AppConfig::instance();
        commandVideo_ = new VideoGLWidget(this);
        commandVideo_->setFixedSize(
                QSize(cfg.commandVideoWidth(), cfg.commandVideoHeight()));
        commandVideo_->setSource(videoHub);
        formColumn->addWidget(commandVideo_, 0, Qt::AlignLeft | Qt::AlignTop);
    }

    // ---- 系统组 ----
    {
        auto* group = new QGroupBox(QString::fromLocal8Bit("系统"), this);
        auto* lay = new QHBoxLayout(group);
        askBtn_ = new QPushButton(QStringLiteral("ask"), group);
        verBtn_ = new QPushButton(QStringLiteral("ver"), group);
        statusBtn_ = new QPushButton(QStringLiteral("status"), group);
        helpBtn_ = new QPushButton(QStringLiteral("help"), group);
        lay->addWidget(askBtn_);
        lay->addWidget(verBtn_);
        lay->addWidget(statusBtn_);
        lay->addWidget(helpBtn_);
        lay->addStretch(1);
        formColumn->addWidget(group);
        connect(askBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("ask"))) {
                emit commandRequested(static_cast<quint16>(wire::Func::Ask), QByteArray());
            }
        });
        connect(verBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("ver"))) {
                emit commandRequested(static_cast<quint16>(wire::Func::Ver), QByteArray());
            }
        });
        connect(statusBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("status"))) {
                emit commandRequested(static_cast<quint16>(wire::Func::Status), QByteArray());
            }
        });
        connect(helpBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("help"))) {
                emit commandRequested(static_cast<quint16>(wire::Func::Help), QByteArray());
            }
        });
    }

    // ---- 安全组 ----
    {
        auto* group = new QGroupBox(QString::fromLocal8Bit("安全"), this);
        auto* lay = new QHBoxLayout(group);
        stopBtn_ = new QPushButton(QString::fromLocal8Bit("stop all 推进器停止"), group);
        estopBtn_ = new QPushButton(QString::fromLocal8Bit("estop 紧急停机"), group);
        emergencyBtn_ = new QPushButton(QString::fromLocal8Bit("emergency 紧急停机（高优先级）"), group);
        estopBtn_->setStyleSheet(QStringLiteral(
                "QPushButton { background:#8c2f2f; color:white; font-weight:bold; }"));
        emergencyBtn_->setStyleSheet(QStringLiteral(
                "QPushButton { background:#b45309; color:white; font-weight:bold; }"));
        lay->addWidget(stopBtn_);
        lay->addWidget(estopBtn_);
        lay->addWidget(emergencyBtn_);
        lay->addStretch(1);
        formColumn->addWidget(group);
        connect(stopBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("stop all"))) {
                emit commandRequested(static_cast<quint16>(wire::Func::StopAll), QByteArray());
            }
        });
        connect(estopBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("estop"))) {
                emit estopRequested();
            }
        });
        connect(emergencyBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("emergency"))) {
                emit emergencyConfirmRequired();
            }
        });
    }

    // ---- 模式开关组（7 个事务开关，与主页共用同一 SafetyStateModel）----
    {
        auto* group = new QGroupBox(QString::fromLocal8Bit("模式"), this);
        auto* lay = new QVBoxLayout(group);

        const struct
        {
            SwitchId id;
            const char* title; // GBK 字面量
        } rows[] = {
            {SwitchId::Safe, "Safe 安全保护"},
            {SwitchId::AttitudeStab, "姿态稳定（Horizontal）"},
            {SwitchId::GlobalEnable, "推进器总使能"},
            {SwitchId::VerticalEnable, "垂直推进使能"},
            {SwitchId::HorizontalEnable, "水平推进使能"},
            {SwitchId::VerticalSync, "垂直同步（Synchronization）"},
            {SwitchId::HorizontalSync, "水平同步（Synchronization）"},
        };
        for (const auto& row : rows) {
            auto* sw = new SwitchButtonWidget(QString::fromLocal8Bit(row.title), group);
            lay->addWidget(sw);
            modeSwitches_.append({row.id, sw});
            connect(sw, &SwitchButtonWidget::toggleRequested, this,
                    [this, row, sw](bool on) {
                        sw->showPending(on); // 乐观显示，回退经刷新链修正
                        onModeSwitchToggled(row.id, on);
                    });
        }

        formColumn->addWidget(group);
    }

    // ---- 查询组 ----
    {
        auto* group = new QGroupBox(QString::fromLocal8Bit("查询"), this);
        auto* grid = new QHBoxLayout(group);

        auto col1 = new QWidget(group);
        auto l1 = new QVBoxLayout(col1);
        servoGetIdSpin_ = new QSpinBox(col1);
        servoGetIdSpin_->setRange(0, wire::kServoCount); // 0=全部；1..10=UI 编号
        servoGetIdSpin_->setValue(0);
        auto b1 = new QPushButton(
                QString::fromLocal8Bit("get servo <0=all|UI 1-10>"), col1);
        l1->addWidget(servoGetIdSpin_);
        l1->addWidget(b1);
        connect(b1, &QPushButton::clicked, this, &CommandPageWidget::sendServoGet);

        auto col2 = new QWidget(group);
        auto l2 = new QVBoxLayout(col2);
        propKindCombo_ = new QComboBox(col2);
        propKindCombo_->addItem(QStringLiteral("base"));
        propKindCombo_->addItem(QStringLiteral("real"));
        propGetIdSpin_ = new QSpinBox(col2);
        propGetIdSpin_->setRange(wire::kVerticalIdFirst, wire::kHorizontalIdLast); // wire 10..15
        propGetIdSpin_->setValue(wire::kVerticalIdFirst);
        auto b2 = new QPushButton(
                QString::fromLocal8Bit("get propeller <wire 10-15> base/real"), col2);
        l2->addWidget(propKindCombo_);
        l2->addWidget(propGetIdSpin_);
        l2->addWidget(b2);
        connect(b2, &QPushButton::clicked, this, &CommandPageWidget::sendPropellerGet);

        auto col3 = new QWidget(group);
        auto l3 = new QVBoxLayout(col3);
        sensorMpuBtn_ = new QPushButton(QStringLiteral("sensor mpu"), col3);
        sensorDypBtn_ = new QPushButton(QStringLiteral("sensor dyp"), col3);
        sensorAllBtn_ = new QPushButton(QStringLiteral("sensor all"), col3);
        l3->addWidget(sensorMpuBtn_);
        l3->addWidget(sensorDypBtn_);
        l3->addWidget(sensorAllBtn_);
        connect(sensorMpuBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("sensor mpu"))) {
                emit commandRequested(static_cast<quint16>(wire::Func::SensorMpu), QByteArray());
            }
        });
        connect(sensorDypBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("sensor dyp"))) {
                emit commandRequested(static_cast<quint16>(wire::Func::SensorDyp), QByteArray());
            }
        });
        connect(sensorAllBtn_, &QPushButton::clicked, this, [this] {
            if (gateSend(QStringLiteral("sensor all"))) {
                emit commandRequested(static_cast<quint16>(wire::Func::SensorAll), QByteArray());
            }
        });

        grid->addWidget(col1);
        grid->addWidget(col2);
        grid->addWidget(col3);
        grid->addStretch(1);
        formColumn->addWidget(group);
    }

    // ---- 高级原始入口 ----
    {
        auto* group = new QGroupBox(
                QString::fromLocal8Bit("高级原始入口（仅无参函数，空载荷）"), this);
        auto* lay = new QHBoxLayout(group);
        rawCombo_ = new QComboBox(group);
        for (const wire::FunctionEntry& entry : wire::FunctionRegistry::all()) {
            if (entry.direction != wire::Direction::Request) {
                continue;
            }
            rawCombo_->addItem(QString::fromLatin1(entry.name),
                               static_cast<uint>(entry.funcId));
        }
        rawSendBtn_ = new QPushButton(QString::fromLocal8Bit("发送"), group);
        lay->addWidget(rawCombo_, 1);
        lay->addWidget(rawSendBtn_);
        formColumn->addWidget(group);
        connect(rawSendBtn_, &QPushButton::clicked, this, &CommandPageWidget::sendRaw);
    }

    formColumn->addStretch(1);
    auto* formHost = new QWidget(this);
    formHost->setLayout(formColumn);
    body->addWidget(formHost, 3);

    // ---- 控制区（复用主页组件；水平滑条行布局防窗口过宽）----
    controlArea_ = new ControlAreaWidget(vm_, safety_, false, Qt::Horizontal, this);
    body->addWidget(controlArea_, 4);

    // 请求被拒（链路不可用等）：乐观显示回到权威状态
    connect(vm_, &ControlViewModel::permissionBlocked, this,
            &CommandPageWidget::refreshModeButtons, Qt::QueuedConnection);

    // ---- 结果表 ----
    table_ = new QTableWidget(0, 5, this);
    table_->setHorizontalHeaderLabels(QStringList()
            << QString::fromLocal8Bit("时间")
            << QString::fromLocal8Bit("命令")
            << QString::fromLocal8Bit("Seq")
            << QString::fromLocal8Bit("状态")
            << QString::fromLocal8Bit("详情（耗时/错误/数据）"));
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setMaximumHeight(200);
    rootLayout->addLayout(body, 1);
    rootLayout->addWidget(table_);

    refreshModeButtons();
}

void CommandPageWidget::refreshModeButtons()
{
    // 7 个事务开关按 SafetyStateModel 权威四态绑定（含 Safe 联动锁定）
    for (const ModeSwitch& ms : modeSwitches_) {
        ms.widget->bind(safety_, ms.id);
    }
    controlArea_->refreshPermissions();
}

void CommandPageWidget::onModeSwitchToggled(SwitchId id, bool on)
{
    // 权限判定（Pending 禁重复点击 / Safe ON 禁关姿态稳定）在模型侧
    if (!safety_->switchToggleAllowed(id, on)) {
        refreshModeButtons(); // 回到权威显示
        return;
    }
    switch (id) {
    case SwitchId::Safe:
        vm_->requestSafeMode(on);
        break;
    case SwitchId::AttitudeStab:
        vm_->requestAttitudeStab(on);
        break;
    case SwitchId::GlobalEnable:
        on ? vm_->requestMoveAll() : vm_->requestStopAll();
        break;
    case SwitchId::VerticalEnable:
        on ? vm_->requestMoveGroup(true) : vm_->requestStopGroup(true);
        break;
    case SwitchId::HorizontalEnable:
        on ? vm_->requestMoveGroup(false) : vm_->requestStopGroup(false);
        break;
    case SwitchId::VerticalSync:
        vm_->requestVerticalSync(on);
        break;
    case SwitchId::HorizontalSync:
        vm_->requestHorizontalSync(on);
        break;
    default:
        break;
    }
}

bool CommandPageWidget::gateSend(const QString& name)
{
    if (!linkAvailable_) {
        appendRowNoop(name);
        return false;
    }
    return true;
}

void CommandPageWidget::appendRowNoop(const QString& name)
{
    const int count = table_->rowCount();
    if (count >= kMaxResultRows) {
        table_->removeRow(0);
    }
    table_->insertRow(table_->rowCount());
    const int r = table_->rowCount() - 1;
    table_->setItem(r, 0, new QTableWidgetItem(QDateTime::currentDateTime().toString(
            QStringLiteral("HH:mm:ss"))));
    table_->setItem(r, 1, new QTableWidgetItem(name));
    table_->setItem(r, 2, new QTableWidgetItem(QStringLiteral("-")));
    table_->setItem(r, 3, new QTableWidgetItem(QString::fromLocal8Bit("未发送")));
    table_->setItem(r, 4, new QTableWidgetItem(QString::fromLocal8Bit("链路不可用")));
}

void CommandPageWidget::setLinkAvailable(bool available)
{
    linkAvailable_ = available;
}

void CommandPageWidget::releaseVideoGl()
{
    if (commandVideo_ != nullptr) {
        commandVideo_->releaseGl(); // 幂等；MainWindow closeEvent 逆序首步调用
    }
}

void CommandPageWidget::onRequestSent(quint16 seq, quint16 funcId)
{
    const wire::FunctionEntry* entry = wire::FunctionRegistry::findByFuncId(funcId);
    if (entry == nullptr) {
        return;
    }
    RequestRow row;
    row.name = QString::fromLatin1(entry->name);
    row.sentAtMs = QDateTime::currentMSecsSinceEpoch();

    const int count = table_->rowCount();
    if (count >= kMaxResultRows) {
        table_->removeRow(0);
    }
    table_->insertRow(table_->rowCount());
    row.tableRow = table_->rowCount() - 1;
    table_->setItem(row.tableRow, 0, new QTableWidgetItem(
            QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    table_->setItem(row.tableRow, 1, new QTableWidgetItem(row.name));
    table_->setItem(row.tableRow, 2, new QTableWidgetItem(QString::number(seq)));
    table_->setItem(row.tableRow, 3, new QTableWidgetItem(QString::fromLocal8Bit("请求中")));
    table_->setItem(row.tableRow, 4, new QTableWidgetItem(QString()));
    table_->scrollToBottom();
    pending_.insert(seq, row);
}

void CommandPageWidget::finishRow(quint16 seq, const QString& state, const QString& detail)
{
    const RequestRow row = pending_.take(seq);
    if (row.tableRow < 0) {
        return;
    }
    QTableWidgetItem* stateItem = table_->item(row.tableRow, 3);
    if (stateItem != nullptr) {
        stateItem->setText(state);
    }
    QTableWidgetItem* detailItem = table_->item(row.tableRow, 4);
    if (detailItem != nullptr) {
        detailItem->setText(detail);
    }
}

void CommandPageWidget::onAck(quint16 seq, quint16 errCode)
{
    const RequestRow row = pending_.value(seq);
    const qint64 elapsed = (row.sentAtMs > 0)
            ? QDateTime::currentMSecsSinceEpoch() - row.sentAtMs : 0;
    finishRow(seq,
              errCode == 0U ? QString::fromLocal8Bit("成功")
                            : QString::fromLocal8Bit("失败(%1)").arg(errCode),
              QString::fromLocal8Bit("耗时 %1ms").arg(elapsed));
}

void CommandPageWidget::onTimeout(quint16 seq)
{
    finishRow(seq, QString::fromLocal8Bit("超时"),
              QString::fromLocal8Bit("未收到 ACK（状态未知）"));
}

void CommandPageWidget::onResponse(quint16 funcId, const QByteArray& payload)
{
    QString text;
    std::vector<qint16> list;
    if (funcId == static_cast<quint16>(wire::Func::ServoGet)
        || funcId == static_cast<quint16>(wire::Func::ServoGetAll)) {
        if (wire::decodeAngleList(payload, list)) {
            QStringList nums;
            for (const qint16 v : list) {
                nums << QString::number(v);
            }
            text = nums.join(QStringLiteral(", "));
        }
    } else if (funcId == static_cast<quint16>(wire::Func::PropellerGetBase)
               || funcId == static_cast<quint16>(wire::Func::PropellerGetReal)) {
        if (wire::decodePropellerList(payload, list)) {
            QStringList nums;
            for (const qint16 v : list) {
                nums << QString::number(v);
            }
            text = nums.join(QStringLiteral(", "));
        }
    }
    if (text.isEmpty()) {
        const int n = qMin(payload.size(), 48);
        QString hex;
        for (int i = 0; i < n; ++i) {
            hex += QStringLiteral("%1 ").arg(static_cast<unsigned char>(payload.at(i)),
                                             2, 16, QLatin1Char('0'));
        }
        if (payload.size() > n) {
            hex += QStringLiteral("...");
        }
        text = hex;
    }
    const int count = table_->rowCount();
    if (count >= kMaxResultRows) {
        table_->removeRow(0);
    }
    table_->insertRow(table_->rowCount());
    const int r = table_->rowCount() - 1;
    table_->setItem(r, 0, new QTableWidgetItem(QDateTime::currentDateTime().toString(
            QStringLiteral("HH:mm:ss"))));
    table_->setItem(r, 1, new QTableWidgetItem(funcName(funcId)));
    table_->setItem(r, 2, new QTableWidgetItem(QStringLiteral("-")));
    table_->setItem(r, 3, new QTableWidgetItem(QString::fromLocal8Bit("数据")));
    table_->setItem(r, 4, new QTableWidgetItem(text));
    table_->scrollToBottom();
}

void CommandPageWidget::sendServoGet()
{
    if (!gateSend(QStringLiteral("get servo"))) {
        return;
    }
    const int value = servoGetIdSpin_->value();
    if (value == 0) {
        // 0 = 全部：ServoGetAll 空载荷（列表查询无逐台 id）
        emit commandRequested(static_cast<quint16>(wire::Func::ServoGetAll), QByteArray());
        return;
    }
    QByteArray payload;
    payload.append(static_cast<char>(wire::servoWireId(value))); // UI 1..10 -> wire 0..9
    emit commandRequested(static_cast<quint16>(wire::Func::ServoGet), payload);
}

void CommandPageWidget::sendPropellerGet()
{
    if (!gateSend(QStringLiteral("get propeller"))) {
        return;
    }
    QByteArray payload;
    payload.append(static_cast<char>(static_cast<quint8>(propGetIdSpin_->value())));
    const quint16 func = (propKindCombo_->currentIndex() == 0)
            ? static_cast<quint16>(wire::Func::PropellerGetBase)
            : static_cast<quint16>(wire::Func::PropellerGetReal);
    emit commandRequested(func, payload);
}

void CommandPageWidget::sendRaw()
{
    const quint16 funcId = static_cast<quint16>(rawCombo_->currentData().toUInt());
    if (!gateSend(funcName(funcId))) {
        return;
    }
    emit commandRequested(funcId, QByteArray());
}

} // namespace salacia
