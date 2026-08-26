#include "RovVizModel.h"

namespace salacia {

RovVizModel::RovVizModel(QObject* parent)
    : QObject(parent)
{
}

void RovVizModel::bindToDataManager()
{
    // 显式 QueuedConnection：发布线程为遥测/主线程，本对象驻留主线程
    connect(&DataManager::instance(), &DataManager::rovStateUpdated,
            this, &RovVizModel::refresh, Qt::QueuedConnection);
    connect(&DataManager::instance(), &DataManager::linkStateChanged,
            this, &RovVizModel::refresh, Qt::QueuedConnection);
}

void RovVizModel::refresh()
{
    cached_ = DataManager::instance().rovState();
    active_ = DataManager::instance().telemetryActive();
    emit stateChanged();
}

} // namespace salacia
