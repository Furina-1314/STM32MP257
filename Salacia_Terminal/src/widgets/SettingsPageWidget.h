#pragma once

#include <QColor>
#include <QWidget>

class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QSpinBox;

namespace salacia {

class AppConfig;

// 设置页：强调色（Win11 个性化风格）+ 运行参数实时编辑 + 保存入 ini
class SettingsPageWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPageWidget(AppConfig& cfg, QWidget* parent = nullptr);

signals:
    void configSaved();

private slots:
    void saveToIni();

private:
    QWidget* buildAccentGroup();
    QWidget* buildParamsGroup();
    QWidget* buildSummaryGroup();
    void applyAccent(const QColor& color);

    AppConfig& cfg_;

    QPushButton* customAccentBtn_ = nullptr;
    QColor currentAccent_;
    QVector<QPushButton*> accentSwatches_;

    QDoubleSpinBox* confSpin_ = nullptr;
    QDoubleSpinBox* nmsSpin_ = nullptr;
    QSpinBox* reqTimeoutSpin_ = nullptr;
    QSpinBox* heartbeatSpin_ = nullptr;
    QSpinBox* sensorStaleSpin_ = nullptr;
    QSpinBox* alarmMaxSpin_ = nullptr;
    QSpinBox* alarmMergeSpin_ = nullptr;
    QSpinBox* anglePrecSpin_ = nullptr;
    QSpinBox* voltagePrecSpin_ = nullptr;
    QSpinBox* distancePrecSpin_ = nullptr;
    QDoubleSpinBox* batteryLowSpin_ = nullptr;
    QDoubleSpinBox* batteryCriticalSpin_ = nullptr;

    QPushButton* saveBtn_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
};

} // namespace salacia
