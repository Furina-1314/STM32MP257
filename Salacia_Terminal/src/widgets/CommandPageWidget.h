#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <QWidget>

#include "core/SafetyStateModel.h"

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;

namespace salacia {

class ControlAreaWidget;
class ControlViewModel;
class SafetyStateModel;
class SwitchButtonWidget;
class VideoFrameHub;
class VideoGLWidget;

// 指令页：左上角小尺寸实时视频（与主页共享同一 VideoFrameHub/单管线）+
// 模式开关（7 个事务开关，与主页共用同一 SafetyStateModel）+
// 控制区（复用主页组件）+ 查询表单 + 受限原始入口
class CommandPageWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CommandPageWidget(ControlViewModel* viewModel,
                               SafetyStateModel* safety,
                               VideoFrameHub* videoHub = nullptr,
                               QWidget* parent = nullptr);

    void onRequestSent(quint16 seq, quint16 funcId);
    void onAck(quint16 seq, quint16 errCode);
    void onTimeout(quint16 seq);
    void onResponse(quint16 funcId, const QByteArray& payload);
    void setLinkAvailable(bool available);
    void refreshModeButtons(); // SafetyStateModel::stateChanged -> 此处
    void releaseVideoGl();     // 关闭流程：提前释放小视频 GL 资源（幂等）

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

private:
    void appendRowNoop(const QString& name);
    void finishRow(quint16 seq, const QString& state, const QString& detail);
    bool gateSend(const QString& name);
    void onModeSwitchToggled(SwitchId id, bool on);

    ControlViewModel* vm_ = nullptr;
    SafetyStateModel* safety_ = nullptr;

    QPushButton* askBtn_ = nullptr;
    QPushButton* verBtn_ = nullptr;
    QPushButton* statusBtn_ = nullptr;
    QPushButton* helpBtn_ = nullptr;

    QPushButton* stopBtn_ = nullptr;
    QPushButton* estopBtn_ = nullptr;
    QPushButton* emergencyBtn_ = nullptr;

    // 模式区 7 个事务开关（Safe/姿态稳定/总使能/垂直使能/水平使能/双同步）
    struct ModeSwitch
    {
        SwitchId id = SwitchId::Safe;
        SwitchButtonWidget* widget = nullptr;
    };
    QVector<ModeSwitch> modeSwitches_;

    QSpinBox* servoGetIdSpin_ = nullptr;
    QComboBox* propKindCombo_ = nullptr;
    QSpinBox* propGetIdSpin_ = nullptr;
    QPushButton* sensorMpuBtn_ = nullptr;
    QPushButton* sensorDypBtn_ = nullptr;
    QPushButton* sensorAllBtn_ = nullptr;

    QComboBox* rawCombo_ = nullptr;
    QPushButton* rawSendBtn_ = nullptr;

    ControlAreaWidget* controlArea_ = nullptr;
    VideoGLWidget* commandVideo_ = nullptr; // 左上角小视频（共享 Hub，可空）

    QTableWidget* table_ = nullptr;
    QMap<quint16, RequestRow> pending_;

    bool linkAvailable_ = false;
};

} // namespace salacia
