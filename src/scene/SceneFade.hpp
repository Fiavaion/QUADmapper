#pragma once

// Scene crossfades (ADR-047 §5, QM-121): a take with a fade time mixes the scene an
// output showed into the scene it shows now. The state is per output, keyed by the
// output's id, and driven by the frame clock (seconds on a steady wall clock, sampled
// once per frame), never by frame count. No GL: the renderer and the audio read the
// mix from here.

#include "core/UUID.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace quadmapper {

// Longest fade a take may ask for (Preferences, OSC); longer requests are clamped.
constexpr float MAX_SCENE_FADE_SECONDS = 60.0f;

// One output's fade from what it showed to the scene it shows now.
struct SceneFade {
    UUID fromSceneId;      // null: the output showed nothing, so the new scene fades in from black
    UUID toSceneId;
    double start = 0.0;    // frame-clock seconds at which the mix was 0
    double seconds = 0.0;  // > 0
    // A take of a third scene during a fade freezes that fade as the outgoing side:
    // fromSceneId and fromNextSceneId mixed at fromNextWeight, the weight
    // fromNextSceneId had on screen at the take. Both keep rendering (video in them
    // plays on). 0: the outgoing side is fromSceneId alone.
    UUID fromNextSceneId;
    float fromNextWeight = 0.0f;

    bool nested() const { return fromNextWeight > 0.0f; }
};

// Weight of the new scene at `now`: 0 at the start, 1 at the end, linear in time (a
// video dissolve). Clamped, so a frame before the start shows the old scene.
inline float sceneFadeMix(const SceneFade& fade, double now) {
    if (!(fade.seconds > 0.0)) return 1.0f;
    const double t = (now - fade.start) / fade.seconds;
    if (t >= 1.0) return 1.0f;
    if (t <= 0.0) return 0.0f;
    return static_cast<float>(t);
}

// Audio gains at a picture mix. Equal power (cos/sin): two different soundtracks are
// uncorrelated, so their powers add and cos² + sin² = 1 keeps the loudness level
// through the fade, where a linear fade dips 3 dB at the middle. The ends are exact:
// 1/0 at mix 0 and 0/1 at mix 1.
struct CrossGain {
    float out = 1.0f;  // the scene fading out
    float in = 0.0f;   // the scene fading in
};

inline CrossGain crossfadeGains(float mix) {
    if (!(mix > 0.0f)) return CrossGain{1.0f, 0.0f};
    if (mix >= 1.0f) return CrossGain{0.0f, 1.0f};
    constexpr double halfPi = 1.57079632679489661923;
    return CrossGain{static_cast<float>(std::cos(mix * halfPi)), static_cast<float>(std::sin(mix * halfPi))};
}

// One scene's share of an output during a fade (a null scene is black).
struct SceneFadeLeg {
    UUID sceneId;
    float weight = 0.0f;
};

// The audio gains of a fade's scenes at `now`: the outgoing side, its frozen second
// scene (gain 0 when the fade is not nested) and the incoming scene. Equal power
// across all three: the incoming scene at crossfadeGains(mix).in, and the outgoing
// side's .out split between its two scenes by their equal-power gains at the frozen
// weight. The squares sum to 1, and a third take carries on exactly the gains the
// interrupted fade had.
inline std::array<SceneFadeLeg, 3> sceneFadeAudioGains(const SceneFade& fade, double now) {
    const CrossGain gains = crossfadeGains(sceneFadeMix(fade, now));
    const CrossGain held = fade.nested() ? crossfadeGains(fade.fromNextWeight) : CrossGain{1.0f, 0.0f};
    return {SceneFadeLeg{fade.fromSceneId, gains.out * held.out},
            SceneFadeLeg{fade.fromNextSceneId, gains.out * held.in},
            SceneFadeLeg{fade.toSceneId, gains.in}};
}

// The fades in progress, one per output at most.
//
// A take during a fade starts from what is on screen:
//   - taking back the scene that is fading out reverses the fade from where it is (the
//     mix continues from its current value, no jump), over the new fade time;
//   - taking any other scene freezes the fade in progress at its current mix, both
//     scenes still playing, and fades from that frozen mix to the new scene: no jump;
//   - a take during such a nested fade first collapses its frozen side into the scene
//     that dominates it (the one that was fading in, on a tie), then follows these
//     rules as if that scene were fading to the incoming one at the same mix. Only the
//     frozen side's minor scene jumps, by at most half the outgoing weight;
//   - a cut (fade time 0) ends the fade: the new scene shows at once;
//   - taking the scene that is already fading in changes nothing.
class SceneFades {
public:
    // A take routed output `outputId` from `from` (null: it showed nothing) to `to` at
    // `now`, with a fade of `seconds` (<= 0 or not finite: a cut). A stored fade that
    // does not end on `from` is stale (the output was re-routed by hand since) and is
    // dropped first: the take starts from `from`, which is what is on screen.
    void begin(const UUID& outputId, const UUID& from, const UUID& to, double now, double seconds) {
        auto it = fades_.find(outputId);
        if (it != fades_.end() && it->second.toSceneId != from) {
            fades_.erase(it);
            it = fades_.end();
        }
        const bool cut = !(seconds > 0.0) || !std::isfinite(seconds);
        if (it != fades_.end() && to == it->second.toSceneId) {
            if (cut) fades_.erase(it);  // a cut completes it; a fade carries on as it was
            return;
        }
        if (cut || from == to) {
            if (it != fades_.end()) fades_.erase(it);
            return;
        }
        SceneFade next{from, to, now, seconds};
        if (it != fades_.end() && sceneFadeMix(it->second, now) < 1.0f) {
            const SceneFade current = collapsed(it->second);
            const float mix = sceneFadeMix(current, now);
            if (to == current.fromSceneId) {
                // Reverse: the new scene is already (1 - mix) on screen.
                next.fromSceneId = current.toSceneId;
                next.start = now - (1.0 - mix) * seconds;
            } else if (mix > 0.0f) {
                // Freeze what is on screen and fade from it.
                next.fromSceneId = current.fromSceneId;
                next.fromNextSceneId = current.toSceneId;
                next.fromNextWeight = mix;
            } else {
                next.fromSceneId = current.fromSceneId;  // nothing else is on screen yet
            }
        }
        fades_[outputId] = next;
    }

    const SceneFade* find(const UUID& outputId) const {
        auto it = fades_.find(outputId);
        return it == fades_.end() ? nullptr : &it->second;
    }

    // The new scene's weight on `outputId` at `now`; 1 when it is not fading.
    float mix(const UUID& outputId, double now) const {
        const SceneFade* fade = find(outputId);
        return fade ? sceneFadeMix(*fade, now) : 1.0f;
    }

    // Drop every fade `keep(outputId, fade)` rejects, and every fade that has finished
    // at `now`. Returns true when one was dropped.
    template <typename Keep>
    bool retire(double now, Keep keep) {
        bool dropped = false;
        for (auto it = fades_.begin(); it != fades_.end();) {
            if (sceneFadeMix(it->second, now) >= 1.0f || !keep(it->first, it->second)) {
                it = fades_.erase(it);
                dropped = true;
            } else {
                ++it;
            }
        }
        return dropped;
    }

    // A scene is going away: fades from, through or to it end (the output shows its
    // current scene, or whatever the deletion routes it to).
    void cancelScene(const UUID& sceneId) {
        for (auto it = fades_.begin(); it != fades_.end();) {
            const SceneFade& fade = it->second;
            if (fade.fromSceneId == sceneId || fade.fromNextSceneId == sceneId || fade.toSceneId == sceneId) {
                it = fades_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void cancelOutput(const UUID& outputId) { fades_.erase(outputId); }
    void clear() { fades_.clear(); }
    bool empty() const { return fades_.empty(); }

private:
    // A nested fade as the plain fade it collapses to: its frozen side becomes the
    // scene that dominates it (the one that was fading in, on a tie), same clock.
    static SceneFade collapsed(const SceneFade& fade) {
        if (!fade.nested()) return fade;
        const UUID& dominant = fade.fromNextWeight >= 0.5f ? fade.fromNextSceneId : fade.fromSceneId;
        return SceneFade{dominant, fade.toSceneId, fade.start, fade.seconds};
    }

    std::unordered_map<UUID, SceneFade> fades_;
};

}  // namespace quadmapper
