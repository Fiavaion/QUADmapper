#pragma once

#include "VideoPlayer.hpp"
#include "core/UUID.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

namespace quadmapper {

/**
 * @brief Manages synchronized playback across multiple video players
 *
 * Videos can be "locked" to the timeline, meaning they will wait for
 * all other locked videos before looping.
 */
class TimelineSyncManager {
public:
    TimelineSyncManager() = default;
    ~TimelineSyncManager() = default;

    /**
     * @brief Register a video player with the sync manager
     * @param id Unique identifier for this player
     * @param player Pointer to the player (not owned)
     */
    void registerPlayer(const UUID& id, VideoPlayer* player);

    /**
     * @brief Unregister a video player
     * @param id Player identifier
     */
    void unregisterPlayer(const UUID& id);

    /**
     * @brief Check if a player is registered
     */
    bool hasPlayer(const UUID& id) const;

    /**
     * @brief Lock a player to the timeline (synced restart)
     * @param id Player identifier
     */
    void lockToTimeline(const UUID& id);

    /**
     * @brief Unlock a player from the timeline
     * @param id Player identifier
     */
    void unlockFromTimeline(const UUID& id);

    /**
     * @brief Check if a player is locked to timeline
     */
    bool isLockedToTimeline(const UUID& id) const;

    /**
     * @brief Get all locked player IDs
     */
    std::vector<UUID> getLockedPlayers() const;

    /**
     * @brief Update all registered players
     * @param deltaTime Time since last update
     *
     * Handles synchronized looping for locked players.
     */
    void update(double deltaTime);

    /**
     * @brief Play all registered players
     */
    void playAll();

    /**
     * @brief Pause all registered players
     */
    void pauseAll();

    /**
     * @brief Stop all registered players
     */
    void stopAll();

    /**
     * @brief Seek all registered players to a position
     */
    void seekAll(double timeSeconds);

    /**
     * @brief Seek all to a progress value
     */
    void seekAllProgress(float progress);

    /**
     * @brief Check if all locked players have finished
     */
    bool areAllLockedFinished() const;

    /**
     * @brief Get the master duration (longest locked video)
     */
    double getMasterDuration() const;

    /**
     * @brief Set whether to use master timeline
     */
    void setUseMasterTimeline(bool use);

    /**
     * @brief Check if using master timeline
     */
    bool isUsingMasterTimeline() const;

private:
    void checkSyncedRestart();

    struct PlayerEntry {
        VideoPlayer* player = nullptr;
        bool lockedToTimeline = false;
        bool waitingForOthers = false;
    };

    std::unordered_map<UUID, PlayerEntry> players_;
    bool useMasterTimeline_ = false;
};

} // namespace quadmapper
