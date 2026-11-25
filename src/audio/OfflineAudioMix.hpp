#pragma once

// Export video: the sound of several video files mixed offline, the way the live show
// plays them (ADR-050): a clip is heard only at 1x speed going forward, so a Ping-Pong
// reverse leg and any speed other than 1.0 are silent, Once is silent after its end,
// and Loop wraps with a short equal-power crossfade (the curve of LoopCrossfade) from
// what the file holds past the end into the next pass.
//
// Uses mapTimeline (video/PlaybackClock), so a target linking this also links
// quadmapper_video. Single-threaded.

#include "core/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace quadmapper {

struct OfflineAudioClip {
    std::filesystem::path path;
    double duration = 0.0;  // the video's duration in seconds (> 0)
    LoopMode loopMode = LoopMode::LOOP;
    float speed = 1.0f;
    float gain = 1.0f;         // the source's volume and mute times the master's
    double startOffset = 0.0;  // clip timeline time at export t = 0
};

class OfflineAudioMix {
public:
    explicit OfflineAudioMix(int sampleRate = 48000, int channels = 2);
    ~OfflineAudioMix();

    OfflineAudioMix(const OfflineAudioMix&) = delete;
    OfflineAudioMix& operator=(const OfflineAudioMix&) = delete;

    /// False (with the reason) if the file has no audio or cannot be opened; skip it.
    bool addClip(const OfflineAudioClip& clip, std::string& error);
    size_t clipCount() const;

    /// Writes `frames` sample frames of the mix starting at export time `t0` seconds to
    /// `out` (interleaved, overwritten). Contiguous calls decode straight on; any other
    /// t0 seeks.
    void render(double t0, size_t frames, float* out);

    /// Length of the crossfade at a Loop wrap.
    static constexpr double LOOP_CROSSFADE_SECONDS = 0.005;

private:
    struct Clip;

    void renderClip(Clip& clip, double t0, size_t frames, float* out);
    // Writes clip sample frames [first, first + count) to `out`; returns how many the
    // file held (the rest are silence).
    size_t readClip(Clip& clip, int64_t first, size_t count, float* out);

    const int sampleRate_;
    const int channels_;
    std::vector<std::unique_ptr<Clip>> clips_;
    std::vector<float> fadeIn_;
    std::vector<float> fadeOut_;
    std::vector<float> scratch_;
};

}  // namespace quadmapper
