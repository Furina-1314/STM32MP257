#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;

namespace salacia {

class ControlAreaWidget;
class ControlViewModel;
class SafetyStateModel;

// 指令页：模式开关 + 控制区（复用主页样式）+ 查询表单 + 受限原始入口
class CommandPageWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CommandPageWidget(ControlViewModel* viewModel,
                               SafetyStateModel* safety,
                               QWidget* parent = nullptr);

    void onRequestSent(quint16 seq, quint16 funcId);
    void onAck(quint16 seq, quint16 errCode);
    void onTimeout(quint16 seq);
    void onResponse(quint16 funcId, const QByteArray& payload);
    void setLinkAvailable(bool available);
    void refreshModeButtons(); // SafetyStateModel::stateChanged -> 此处

    struct RequestRow
    {
        int tableRow = -1;
        QString name;
        qint64 sentAtMs = 0;
    };

signals:
    void commandRequested(quint16 funcId, const QByteArray& payload);
    void estopRequested();
    void emergencyConfirmRequired(); // 二次确认弹窗由 MainWindow 处理

private slots:
    void sendServoGet();
    void sendPropellerGet();
    void sendRaw();
    void safeToggled();
    void horizontalToggled();

private:
    void appendRowNoop(const QString& name);
    void finishRow(quint16 seq, const QString& state, const QString& detail);
    bool gateSend(const QString& name);

    ControlViewModel* vm_ = nullptr;
    SafetyStateModel* safety_ = nullptr;

    QPushButton* askBtn_ = nullptr;
    QPushButton* verBtn_ = nullptr;
    QPushButton* statusBtn_ = nullptr;
    QPushButton* helpBtn_ = nullptr;

    QPushButton* stopBtn_ = nullptr;
    QPushButton* estopBtn_ = nullptr;
    QPushButton* emergencyBtn_ = nullptr;

    // SwitchButton（QCheckBox + isSwitchButton 属性）+ ProgressRing 请求指示
    QCheckBox* safeSwitch_ = nullptr;
    QCheckBox* horizontalSwitch_ = nullptr;
    QProgressBar* safeRing_ = nullptr;
    QProgressBar* horizontalRing_ = nullptr;
    QLabel* safeStatusLabel_ = nullptr;
    QLabel* horizontalStatusLabel_ = nullptr;

    QSpinBox* servoGetIdSpin_ = nullptr;
    QComboBox* propKindCombo_ = nullptr;
    QSpinBox* propGetIdSpin_ = nullptr;
    QPushButton* sensorMpuBtn_ = nullptr;
    QPushButton* sensorDypBtn_ = nullptr;
    QPushButton* sensorAllBtn_ = nullptr;

    QComboBox* rawCombo_ = nullptr;
    QPushButton* rawSendBtn_ = nullptr;

    ControlAreaWidget* controlArea_ = nullptr;

    QTableWidget* table_ = nullptr;
    QMap<quint16, RequestRow> pending_;

    bool linkAvailable_ = false;
};

} // namespace salacia
