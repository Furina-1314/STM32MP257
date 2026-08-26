#include "DataManager.h"

#include <utility>

namespace salacia {

DataManager& DataManager::instance()
{
    static DataManager manager; // Meyers 单例：留在主线程（GUI 线程）
    return manager;
}

DataManager::DataManager(QObject* parent)
    : QObject(parent)
{
}

DataManager::~DataManager() = default;

void DataManager::setRovState(const RovState& state)
{
    {
        const std::unique_lock<std::shared_mutex> lock(rovStateMutex_);
        rovState_ = state;
    }
    emit rovStateUpdated();
}

RovState DataManager::rovState() const
{
    const std::shared_lock<std::shared_mutex> lock(rovStateMutex_);
    return rovState_;
}

void DataManager::setDetections(const std::vector<Detection>& items)
{
    {
        const std::unique_lock<std::shared_mutex> lock(detectionsMutex_);
        detections_ = items;
    }
    emit detectionsUpdated();
}

std::vector<Detection> DataManager::detections() const
{
    const std::shared_lock<std::shared_mutex> lock(detectionsMutex_);
    return detections_;
}

void DataManager::setVideoStats(const VideoStats& stats)
{
    {
        const std::unique_lock<std::shared_mutex> lock(videoStatsMutex_);
        videoStats_ = stats;
    }
    emit videoStatsUpdated();
}

VideoStats DataManager::videoStats() const
{
    const std::shared_lock<std::shared_mutex> lock(videoStatsMutex_);
    return videoStats_;
}

void DataManager::setVideoActive(bool on)
{
    if (videoActive_.exchange(on, std::memory_order_acq_rel) != on) {
        emit linkStateChanged();
    }
}

void DataManager::setTelemetryActive(bool on)
{
    if (telemetryActive_.exchange(on, std::memory_order_acq_rel) != on) {
        emit linkStateChanged();
    }
}

void DataManager::setSshConnected(bool on)
{
    if (sshConnected_.exchange(on, std::memory_order_acq_rel) != on) {
        emit linkStateChanged();
    }
}

} // namespace salacia
