#include "AudioDriftController.hpp"

#include <cmath>

namespace quadmapper {

AudioDriftDecision AudioDriftController::update(double drift) {
    AudioDriftDecision decision;
    if (!std::isfinite(drift)) {
        reset();
        return decision;
    }

    if (std::abs(drift) > HARD_THRESHOLD) {
        if (++framesOverHard_ >= HARD_CONFIRM_FRAMES) {
            reset();
            decision.hardResync = true;
        } else {
            decision.correction = correction_;  // hold the current correction while confirming
        }
        return decision;
    }
    framesOverHard_ = 0;

    smoothed_ = primed_ ? smoothed_ + SMOOTHING * (drift - smoothed_) : drift;
    primed_ = true;
    if (std::abs(smoothed_) > SOFT_THRESHOLD) {
        correction_ = smoothed_ > 0.0 ? 1 : -1;
    } else {
        correction_ = 0;
    }
    decision.correction = correction_;
    return decision;
}

void AudioDriftController::reset() {
    smoothed_ = 0.0;
    primed_ = false;
    correction_ = 0;
    framesOverHard_ = 0;
}

}  // namespace quadmapper
