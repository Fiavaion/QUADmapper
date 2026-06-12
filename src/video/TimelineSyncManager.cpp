#include "TimelineSyncManager.hpp"

namespace quadmapper {

void TimelineSyncManager::registerPlayer(const UUID& id, VideoPlayer* player) {
    if (!player) return;

    PlayerEntry entry;
    entry.player = player;
    entry.lockedToTimeline = false;
    entry.waitingForOthers = false;

    players_[id] = entry;
}

void TimelineSyncManager::unregisterPlayer(const UUID& id) {
    players_.erase(id);
}

bool TimelineSyncManager::hasPlayer(const UUID& id) const {
    return players_.find(id) != players_.end();
}

void TimelineSyncManager::lockToTimeline(const UUID& id) {
    auto it = players_.find(id);
    if (it != players_.end()) {
        it->second.lockedToTimeline = true;
    }
}

void TimelineSyncManager::unlockFromTimeline(const UUID& id) {
    auto it = players_.find(id);
    if (it != players_.end()) {
        it->second.lockedToTimeline = false;
        it->second.waitingForOthers = false;
    }
}

bool TimelineSyncManager::isLockedToTimeline(const UUID& id) const {
    auto it = players_.find(id);
    if (it != players_.end()) {
        return it->second.lockedToTimeline;
    }
    return false;
}

std::vector<UUID> TimelineSyncManager::getLockedPlayers() const {
    std::vector<UUID> result;
    for (const auto& [id, entry] : players_) {
        if (entry.lockedToTimeline) {
            result.push_back(id);
        }
    }
    return result;
}

void TimelineSyncManager::update(double deltaTime) {
    // Update all players
    for (auto& [id, entry] : players_) {
        if (entry.player && entry.player->isLoaded()) {
            entry.player->update(deltaTime);

            // Check if this locked player has finished
            if (entry.lockedToTimeline && entry.player->getState().isFinished()) {
                entry.waitingForOthers = true;
            }
        }
    }

    // Check for synchronized restart
    checkSyncedRestart();
}

void TimelineSyncManager::checkSyncedRestart() {
    // Count locked players and how many are waiting
    int lockedCount = 0;
    int waitingCount = 0;

    for (const auto& [id, entry] : players_) {
        if (entry.lockedToTimeline && entry.player && entry.player->isLoaded()) {
            lockedCount++;
            if (entry.waitingForOthers || entry.player->getState().isFinished()) {
                waitingCount++;
            }
        }
    }

    // If all locked players are waiting, restart them all
    if (lockedCount > 0 && waitingCount == lockedCount) {
        for (auto& [id, entry] : players_) {
            if (entry.lockedToTimeline && entry.player) {
                entry.player->seek(0.0);
                entry.player->play();
                entry.waitingForOthers = false;
            }
        }
    }
}

void TimelineSyncManager::playAll() {
    for (auto& [id, entry] : players_) {
        if (entry.player) {
            entry.player->play();
        }
    }
}

void TimelineSyncManager::pauseAll() {
    for (auto& [id, entry] : players_) {
        if (entry.player) {
            entry.player->pause();
        }
    }
}

void TimelineSyncManager::stopAll() {
    for (auto& [id, entry] : players_) {
        if (entry.player) {
            entry.player->stop();
            entry.waitingForOthers = false;
        }
    }
}

void TimelineSyncManager::seekAll(double timeSeconds) {
    for (auto& [id, entry] : players_) {
        if (entry.player) {
            entry.player->seek(timeSeconds);
            entry.waitingForOthers = false;
        }
    }
}

void TimelineSyncManager::seekAllProgress(float progress) {
    for (auto& [id, entry] : players_) {
        if (entry.player) {
            entry.player->seekProgress(progress);
            entry.waitingForOthers = false;
        }
    }
}

bool TimelineSyncManager::areAllLockedFinished() const {
    for (const auto& [id, entry] : players_) {
        if (entry.lockedToTimeline && entry.player) {
            if (!entry.player->getState().isFinished() && !entry.waitingForOthers) {
                return false;
            }
        }
    }
    return true;
}

double TimelineSyncManager::getMasterDuration() const {
    double maxDuration = 0.0;

    for (const auto& [id, entry] : players_) {
        if (entry.lockedToTimeline && entry.player && entry.player->isLoaded()) {
            double duration = entry.player->getState().duration;
            if (duration > maxDuration) {
                maxDuration = duration;
            }
        }
    }

    return maxDuration;
}

void TimelineSyncManager::setUseMasterTimeline(bool use) {
    useMasterTimeline_ = use;
}

bool TimelineSyncManager::isUsingMasterTimeline() const {
    return useMasterTimeline_;
}

} // namespace quadmapper
