#pragma once

// The Project tab's "(modified)" label. It follows the one rule the unsaved-changes
// prompt uses (ProjectSession::isProjectDirty): the editor no longer serializes to what
// was last saved or loaded. So opening a show, or editing and undoing back to the
// saved state, never reads as modified, however many edit calls ran on the way.
// Serializing the show every frame would cost too much, so the answer is
// refreshed at most every REFRESH_SECONDS, and at once after invalidate().

#include <functional>

namespace quadmapper {

class UnsavedIndicator {
public:
    static constexpr double REFRESH_SECONDS = 0.5;

    // Whether the label shows at time `now` (seconds); `dirty` is the snapshot check.
    bool shown(double now, const std::function<bool()>& dirty) {
        if (stale_ || now - checkedAt_ >= REFRESH_SECONDS || now < checkedAt_) {
            shown_ = dirty();
            checkedAt_ = now;
            stale_ = false;
        }
        return shown_;
    }

    // The saved state or the show changed in one step (a save, load, undo): ask again.
    void invalidate() { stale_ = true; }

private:
    bool shown_ = false;
    bool stale_ = true;
    double checkedAt_ = 0.0;
};

}  // namespace quadmapper
