#include "SyncedAudioSource.hpp"

#include <algorithm>
#include <cmath>

namespace quadmapper {

namespace {

void silence(float* output, size_t frames) { std::fill(output, output + frames * 2, 0.0f); }

// Linear gain ramp over `frames` stereo frames, from `from` to `to`.
void ramp(float* output, size_t frames, float from, float to) {
    const float span = to - from;
    const float count = static_cast<float>(frames);
    for (size_t i = 0; i < frames; ++i) {
        const float gain = from + span * (static_cast<float>(i + 1) / count);  // exact at the end
        output[i * 2] *= gain;
        output[i * 2 + 1] *= gain;
    }
}

}  // namespace

SyncedAudioSource::SyncedAudioSource(std::shared_ptr<AudioBuffer> stereoBuffer, int sampleRate)
    : stream_(std::move(stereoBuffer))
    , sampleRate_(sampleRate > 0 ? sampleRate : 48000) {
    setBuffer(stream_);
}

double SyncedAudioSource::nextTimeline() const {
    const uint64_t consumed = stream_->readPosition() - segmentStart_;
    return segmentTimeline_ + static_cast<double>(consumed / 2) / sampleRate_;
}

size_t SyncedAudioSource::fillFromStream(float* output, size_t frameCount) {
    const int correction = correction_.load(std::memory_order_relaxed);
    if (correction == 0) untilCorrection_ = CORRECTION_INTERVAL;

    size_t done = 0;
    while (done < frameCount) {
        const size_t chunk = correction == 0 ? frameCount - done
                                             : std::min(frameCount - done, untilCorrection_);
        const size_t got = stream_->read(output + done * 2, chunk * 2) / 2;
        done += got;
        if (correction != 0) untilCorrection_ -= got;
        if (got < chunk) break;  // ran dry
        if (correction != 0 && untilCorrection_ == 0) {
            if (correction > 0) {
                // Repeat the last frame: time passes, the stream does not advance.
                if (done == frameCount) break;  // no room left; repeat first thing next time
                const float* previous = done > 0 ? output + (done - 1) * 2 : lastFrame_;
                output[done * 2] = previous[0];
                output[done * 2 + 1] = previous[1];
                ++done;
            } else {
                float dropped[2];
                stream_->read(dropped, 2);  // skip one frame
            }
            untilCorrection_ = CORRECTION_INTERVAL;
            ++corrections_;
        }
    }
    if (done > 0) {
        lastFrame_[0] = output[(done - 1) * 2];
        lastFrame_[1] = output[(done - 1) * 2 + 1];
    }
    return done;
}

void SyncedAudioSource::report(size_t frameCount, const AudioRenderContext& context) {
    AudioPlayhead head;
    head.generation = adoptedGeneration_;
    head.aligned = aligned_;
    head.atEnd = atEnd_;
    head.nextTimeline = nextTimeline();
    head.nextWallNs = context.wallNs +
                      static_cast<int64_t>(static_cast<double>(frameCount) * 1e9 / sampleRate_);
    head.callbacks = callbacks_;
    head.underruns = underruns_;
    head.corrections = corrections_;
    playhead_.store(head);
}

void SyncedAudioSource::pull(float* output, size_t frameCount, const AudioRenderContext& context) {
    ++callbacks_;
    // Callbacks stopped for a while (the device was switched, rerouted or lost): what is
    // left in the buffer is stale, so line up afresh instead of carrying on.
    const int64_t bufferNs = static_cast<int64_t>(static_cast<double>(frameCount) * 1e9 / sampleRate_);
    if (lastCallbackNs_ != 0 && context.wallNs - lastCallbackNs_ > std::max<int64_t>(4 * bufferNs, 50000000)) {
        aligned_ = false;
    }
    lastCallbackNs_ = context.wallNs;
    transport_.tryLoad(transportCache_);  // on a torn read keep the last good anchor
    AudioSegment segment;
    const bool newSegment = segment_.tryLoad(segment) && segment.generation != 0 &&
                            segment.generation != adoptedGeneration_;
    const bool wanted = transportCache_.audible && transportCache_.generation == adoptedGeneration_;

    // Stopping (pause, non-1x, reverse, a seek waiting on its new segment): fade the
    // stream out over this buffer rather than cutting it.
    if (aligned_ && (!wanted || newSegment)) {
        const size_t got = fillFromStream(output, frameCount);
        silence(output + got * 2, frameCount - got);
        ramp(output, got, 1.0f, 0.0f);
        aligned_ = false;
        report(frameCount, context);
        return;
    }

    if (newSegment) {
        // Skip what is left of the previous run. If the stream was already read past
        // the start (the samples arrived before the segment was seen), keep going:
        // nextTimeline() still counts from the segment's start.
        stream_->discardTo(segment.startPosition);
        adoptedGeneration_ = segment.generation;
        segmentStart_ = segment.startPosition;
        segmentTimeline_ = segment.timeline;
        atEnd_ = false;
    }

    if (!transportCache_.audible || transportCache_.generation != adoptedGeneration_) {
        silence(output, frameCount);
        report(frameCount, context);
        return;
    }

    size_t offset = 0;
    bool fadeIn = false;
    if (!aligned_) {
        // The frame written now is heard outputLatency later: it must be the sample
        // at transport time (now + latency).
        const double target = transportCache_.timeline +
            static_cast<double>(context.wallNs - transportCache_.wallNs) * 1e-9 +
            context.outputLatency;
        const double lagFrames = (target - nextTimeline()) * sampleRate_;
        if (lagFrames >= 0.5) {
            // Behind: skip to the target sample, if it has been decoded.
            const uint64_t skip = static_cast<uint64_t>(std::llround(lagFrames));
            const uint64_t availableFrames = stream_->available() / 2;
            if (availableFrames < skip) {
                stream_->discardTo(stream_->readPosition() + availableFrames * 2);
                silence(output, frameCount);
                report(frameCount, context);
                return;
            }
            stream_->discardTo(stream_->readPosition() + skip * 2);
        } else if (lagFrames <= -0.5) {
            // Ahead: the stream starts later; silence until then.
            const uint64_t lead = static_cast<uint64_t>(std::llround(-lagFrames));
            if (lead >= frameCount) {
                silence(output, frameCount);
                report(frameCount, context);
                return;
            }
            offset = static_cast<size_t>(lead);
            silence(output, offset);
        }
        aligned_ = true;
        fadeIn = true;
        untilCorrection_ = CORRECTION_INTERVAL;
    }

    const size_t got = fillFromStream(output + offset * 2, frameCount - offset);
    if (fadeIn) ramp(output + offset * 2, got, 0.0f, 1.0f);
    if (offset + got < frameCount) {
        silence(output + (offset + got) * 2, frameCount - offset - got);
        if (stream_->readPosition() >= streamEnd_.load(std::memory_order_acquire)) {
            atEnd_ = true;  // the pass is over: silence, still in step
        } else {
            aligned_ = false;  // line up again once the decoder has caught up
            ++underruns_;
        }
    }
    report(frameCount, context);
}

}  // namespace quadmapper
