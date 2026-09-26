#pragma once

// A stand-in playable source for app-logic tests: records what was asked of it. It has
// zero size, so SourceManager::registerSource creates no texture and no GL is needed.
// Locking follows PlaybackTransport: locking sets it playing, and while locked its own
// seek and speed are ignored.

#include "source/TextureSource.hpp"

#include <string>
#include <utility>

namespace quadmapper {

class FakePlayable : public PlayableSource {
public:
    explicit FakePlayable(std::string name) : name_(std::move(name)) {}
    const UUID& getId() const override { return id_; }
    std::string getName() const override { return name_; }
    SourceType getType() const override { return SourceType::VIDEO_FILE; }
    std::string getTypeName() const override { return "Fake video"; }
    bool initialize() override { return true; }
    void shutdown() override {}
    bool isReady() const override { return true; }
    bool update(double) override { return false; }
    bool hasNewFrame() const override { return false; }
    const FrameData* getCurrentFrame() const override { return nullptr; }
    void markFrameConsumed() override {}
    SourceCapabilities getCapabilities() const override { return {}; }
    int getWidth() const override { return 0; }
    int getHeight() const override { return 0; }

    void play() override { state = PlaybackState::PLAYING; ++plays; }
    void pause() override {
        if (state == PlaybackState::PLAYING) state = PlaybackState::PAUSED;
        ++pauses;
    }
    void stop() override { state = PlaybackState::STOPPED; }
    void togglePlayPause() override {}
    bool canSeek() const override { return true; }
    void seek(double t) override {
        ++seeks;
        if (!locked) time = t;
    }
    void seekProgress(float) override {}
    PlaybackState getPlaybackState() const override { return state; }
    double getCurrentTime() const override { return time; }
    double getDuration() const override { return 10.0; }
    float getProgress() const override { return 0.0f; }
    void setSpeed(float s) override { speed = s; }
    float getSpeed() const override { return speed; }
    void setLoopMode(LoopMode mode) override { loopMode = mode; }
    LoopMode getLoopMode() const override { return loopMode; }
    std::string getLoopModeNotice() const override { return {}; }
    void setTimelineLocked(bool l, const PlaybackClock&) override {
        if (l && !locked) state = PlaybackState::PLAYING;
        locked = l;
    }
    bool isTimelineLocked() const override { return locked; }
    void followTimeline(const PlaybackClock&) override {}
    const PlaybackClock& getClock() const override { return clock_; }
    PlaybackTimePoint getClockNow() const override { return {}; }

    PlaybackState state = PlaybackState::PAUSED;
    int plays = 0, pauses = 0, seeks = 0;
    double time = 5.0;
    float speed = 1.0f;
    LoopMode loopMode = LoopMode::LOOP;
    bool locked = false;

private:
    PlaybackClock clock_;
    UUID id_ = UUID::generate();
    std::string name_;
};

}  // namespace quadmapper
