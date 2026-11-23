#include "OfflineAudioMix.hpp"

#include "AudioDecoder.hpp"
#include "AudioFrame.hpp"
#include "video/PlaybackClock.hpp"

#include <algorithm>
#include <cmath>

namespace quadmapper {

namespace {

// Speeds this close to 1 count as 1x (the only speed that is heard, ADR-050).
constexpr double UNIT_SPEED_TOLERANCE = 1e-6;
// A forward jump up to this long is decoded through rather than sought.
constexpr double MAX_DECODE_THROUGH_SECONDS = 1.0;

}  // namespace

struct OfflineAudioMix::Clip {
    OfflineAudioClip info;
    AudioDecoder decoder;
    std::vector<float> buffer;  // decoded interleaved frames from bufferStart on
    int64_t bufferStart = 0;    // clip sample index of buffer's first frame
    bool positioned = false;    // a seek has set the decode position
    bool awaitingStart = false; // the next decoded frame's pts sets bufferStart
    bool atEnd = false;
    int64_t durationFrames = 0;
};

OfflineAudioMix::OfflineAudioMix(int sampleRate, int channels)
    : sampleRate_(std::max(1, sampleRate)), channels_(std::clamp(channels, 1, 2)) {
    // The equal-power curve LoopCrossfade uses live.
    const size_t fade = std::max<size_t>(1, static_cast<size_t>(std::lround(LOOP_CROSSFADE_SECONDS * sampleRate_)));
    const double quarter = std::acos(0.0);  // pi / 2
    fadeIn_.resize(fade);
    fadeOut_.resize(fade);
    for (size_t i = 0; i < fade; ++i) {
        const double t = (static_cast<double>(i) + 0.5) / static_cast<double>(fade);
        fadeIn_[i] = static_cast<float>(std::sin(t * quarter));
        fadeOut_[i] = static_cast<float>(std::cos(t * quarter));
    }
}

OfflineAudioMix::~OfflineAudioMix() = default;

size_t OfflineAudioMix::clipCount() const { return clips_.size(); }

bool OfflineAudioMix::addClip(const OfflineAudioClip& clip, std::string& error) {
    if (!(clip.duration >= MIN_WRAP_DURATION)) {
        error = "The clip has no known duration.";
        return false;
    }
    auto state = std::make_unique<Clip>();
    if (!state->decoder.open(clip.path, sampleRate_, channels_)) {
        error = "The file has no sound that can be decoded.";
        return false;
    }
    state->info = clip;
    state->durationFrames = std::llround(clip.duration * sampleRate_);
    clips_.push_back(std::move(state));
    return true;
}

void OfflineAudioMix::render(double t0, size_t frames, float* out) {
    if (!out || frames == 0) return;
    const size_t samples = frames * static_cast<size_t>(channels_);
    std::fill(out, out + samples, 0.0f);
    for (auto& clip : clips_) renderClip(*clip, t0, frames, out);
    for (size_t i = 0; i < samples; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

void OfflineAudioMix::renderClip(Clip& clip, double t0, size_t frames, float* out) {
    const OfflineAudioClip& info = clip.info;
    if (std::abs(static_cast<double>(info.speed) - 1.0) > UNIT_SPEED_TOLERANCE || info.gain == 0.0f) return;

    const double rate = static_cast<double>(sampleRate_);
    const double base = info.startOffset + t0;  // the clip timeline at frame 0 (speed is 1)
    const size_t ch = static_cast<size_t>(channels_);
    size_t i = 0;
    while (i < frames) {
        const double timeline = base + static_cast<double>(i) / rate;
        if (timeline < 0.0) {  // before the clip starts
            i = std::max(i + 1, static_cast<size_t>(std::ceil(-base * rate)));
            continue;
        }
        const ClipPosition position = mapTimeline(timeline, info.duration, info.loopMode);
        if (position.ended) return;  // Once, past the end

        // The pass ends at the clip's end (Once) or the next leg boundary.
        const double passEnd = info.loopMode == LoopMode::ONCE
                                   ? info.duration
                                   : static_cast<double>(position.leg + 1) * info.duration;
        const double endExact = std::ceil((passEnd - base) * rate);
        size_t end = endExact >= static_cast<double>(frames) ? frames : static_cast<size_t>(std::max(0.0, endExact));
        end = std::max(end, i + 1);
        const size_t count = end - i;
        if (position.direction < 0) {  // Ping-Pong reverse leg: silent
            i = end;
            continue;
        }

        const int64_t first = std::llround(position.position * rate);
        float* dst = out + i * ch;

        // Loop: the first frames of a pass fade in over what the file holds past the end.
        const int64_t fadeFrames = std::min<int64_t>(static_cast<int64_t>(fadeIn_.size()), clip.durationFrames);
        if (info.loopMode == LoopMode::LOOP && position.leg > 0 && first < fadeFrames && first >= 0) {
            const int64_t fadeEnd = std::min<int64_t>(fadeFrames, first + static_cast<int64_t>(count));
            // From the frame before the wrap, so a continuation that runs out holds its
            // last value instead of stepping to silence.
            const size_t span = static_cast<size_t>(fadeEnd) + 1;
            std::vector<float> continuation(span * ch);
            const size_t real = readClip(clip, clip.durationFrames - 1, span, continuation.data());
            for (size_t k = std::max<size_t>(real, 1); k < span; ++k) {
                for (size_t c = 0; c < ch; ++c) continuation[k * ch + c] = continuation[(k - 1) * ch + c];
            }
            scratch_.resize(count * ch);
            readClip(clip, first, count, scratch_.data());
            for (size_t k = 0; k < count; ++k) {
                const int64_t offset = first + static_cast<int64_t>(k);
                float in = 1.0f, outGain = 0.0f;
                const float* tail = nullptr;
                if (offset < fadeEnd) {
                    in = fadeIn_[static_cast<size_t>(offset * static_cast<int64_t>(fadeIn_.size()) / fadeFrames)];
                    outGain = fadeOut_[static_cast<size_t>(offset * static_cast<int64_t>(fadeIn_.size()) / fadeFrames)];
                    tail = &continuation[static_cast<size_t>(offset + 1) * ch];
                }
                for (size_t c = 0; c < ch; ++c) {
                    const float mixed = scratch_[k * ch + c] * in + (tail ? tail[c] * outGain : 0.0f);
                    dst[k * ch + c] += mixed * info.gain;
                }
            }
        } else {
            scratch_.resize(count * ch);
            readClip(clip, first, count, scratch_.data());
            for (size_t k = 0; k < count * ch; ++k) dst[k] += scratch_[k] * info.gain;
        }
        i = end;
    }
}

size_t OfflineAudioMix::readClip(Clip& clip, int64_t first, size_t count, float* out) {
    const size_t ch = static_cast<size_t>(channels_);
    std::fill(out, out + count * ch, 0.0f);
    if (first < 0) {
        const size_t skip = static_cast<size_t>(std::min<int64_t>(-first, static_cast<int64_t>(count)));
        out += skip * ch;
        count -= skip;
        first = 0;
        if (count == 0) return 0;
    }
    const int64_t last = first + static_cast<int64_t>(count);
    auto bufferEnd = [&] { return clip.bufferStart + static_cast<int64_t>(clip.buffer.size() / ch); };

    const int64_t decodeThrough = std::llround(MAX_DECODE_THROUGH_SECONDS * sampleRate_);
    if (!clip.positioned || first < clip.bufferStart || first > bufferEnd() + decodeThrough) {
        clip.decoder.seek(static_cast<double>(first) / sampleRate_);  // lands early; trimmed below
        clip.buffer.clear();
        clip.positioned = true;
        clip.awaitingStart = true;
        clip.atEnd = false;
    }
    while (!clip.atEnd && (clip.awaitingStart || bufferEnd() < last)) {
        const AudioFrame frame = clip.decoder.decodeNextFrame();
        if (!frame.isValid()) {
            clip.atEnd = true;
            break;
        }
        if (clip.awaitingStart) {
            clip.bufferStart = std::llround(frame.pts * sampleRate_);
            clip.awaitingStart = false;
        }
        clip.buffer.insert(clip.buffer.end(), frame.samples.begin(), frame.samples.end());
        // Drop what lies before the request as it arrives, so a long decode-through
        // does not hold it all.
        if (clip.bufferStart < first) {
            const int64_t drop = std::min<int64_t>(first - clip.bufferStart, static_cast<int64_t>(clip.buffer.size() / ch));
            clip.buffer.erase(clip.buffer.begin(), clip.buffer.begin() + static_cast<ptrdiff_t>(drop * ch));
            clip.bufferStart += drop;
        }
    }

    const int64_t from = std::max(first, clip.bufferStart);
    const int64_t to = std::min(last, bufferEnd());
    if (to <= from) return 0;
    std::copy(clip.buffer.begin() + static_cast<ptrdiff_t>((from - clip.bufferStart) * ch),
              clip.buffer.begin() + static_cast<ptrdiff_t>((to - clip.bufferStart) * ch),
              out + static_cast<size_t>(from - first) * ch);
    return static_cast<size_t>(to - from);
}

}  // namespace quadmapper
