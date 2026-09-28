#pragma once

namespace quadmapper {

// What to do about one source's audio this frame (ADR-050 §2).
struct AudioDriftDecision {
    bool hardResync = false;  // flush, seek the decoder to the clock and line up again
    int correction = 0;       // +1 repeat a sample per 1000 (audio ahead), -1 drop one (behind)
};

/**
 * @brief Keeps a source's audio on its playback clock, from one drift sample per frame
 *
 * drift = audio time - clock time (seconds; positive = the audio is ahead):
 *   |drift| > 80 ms          hard resync (two frames in a row, so one late callback
 *                            timestamp cannot trigger it);
 *   2 ms < |drift| <= 80 ms  soft correction: drop or repeat one sample in 1000
 *                            (0.1 %, inaudible) until the drift is back within 2 ms;
 *   |drift| <= 2 ms          nothing.
 * Callback timestamps jitter by a millisecond or so, so the soft decision uses a
 * smoothed drift (exponential average over roughly ten frames). Pure logic, no clock.
 */
class AudioDriftController {
public:
    static constexpr double SOFT_THRESHOLD = 0.002;
    static constexpr double HARD_THRESHOLD = 0.080;
    static constexpr int HARD_CONFIRM_FRAMES = 2;
    static constexpr double SMOOTHING = 0.1;  // weight of the newest sample

    AudioDriftDecision update(double drift);

    // Forget the history (after a resync, or while there is nothing to measure).
    void reset();

    double smoothedDrift() const { return smoothed_; }
    int correction() const { return correction_; }

private:
    double smoothed_ = 0.0;
    bool primed_ = false;
    int correction_ = 0;
    int framesOverHard_ = 0;
};

}  // namespace quadmapper
