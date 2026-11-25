#pragma once

// Compositions (owner 2026-11-23, ADR-061): one source made of other sources, put on
// a quad like any video. A list of cues, each a source (a video, an image, a
// generator), switched with a crossfade.
// - Timeline: each cue holds for its time, then the next one fades in (looping, or
//   holding on the last).
// - Switch: a cue changes only when told: Next, Previous or Cue n, from the
//   Compositions panel, OSC or MIDI (/qm/composition/<name>/...).
// The picture is drawn on the GPU into the composition's own texture, once a frame,
// after every other source has updated (SourceManager::updateAll).

#include "TextureSource.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace quadmapper {

class Framebuffer;
class ShaderProgram;
class Texture;

enum class CompositionMode { Timeline, Switch };

struct CompositionCue {
    UUID sourceId;              // null: black
    double holdSeconds = 5.0;   // Timeline: how long it shows before the next fades in
    // Not saved: a video added before its file opened takes the clip's length as its
    // hold once that is known (advance).
    bool holdFromClip = false;
};

struct CompositionSettings {
    std::string name = "Composition";
    CompositionMode mode = CompositionMode::Timeline;
    std::vector<CompositionCue> cues;
    double fadeSeconds = 0.5;   // crossfade into a cue; 0 = cut
    bool loop = true;           // Timeline: after the last cue, the first again
    // A video cue starts from its beginning when it comes on and pauses once faded
    // out. Off: every clip keeps running and a switch only changes what shows.
    bool restartOnCue = true;
    int width = 1920;           // the composition's picture; cues are stretched to it
    int height = 1080;
};

// What a step started or finished, for the clip transport.
struct CompositionEvents {
    int started = -1;          // the cue that came on (the last, if several)
    std::vector<int> faded;    // the cues whose fade out ended or was cut short
};

// The cue logic, GL-free (unit-tested).
class CompositionPlayhead {
public:
    int current() const { return current_; }
    int previous() const { return previous_; }  // fading out, -1 = none
    double timeInCue() const { return inCue_; }
    bool running() const { return running_; }
    // How far the current cue has faded in: 1 once no cue fades out.
    double fadeMix(const CompositionSettings& s) const;

    // Bring cue `index` on (clamped), fading from the one showing. The same cue
    // again restarts it.
    void cue(int index, const CompositionSettings& s);
    void next(const CompositionSettings& s);
    void prev(const CompositionSettings& s);
    void play() { running_ = true; }
    void pause() { running_ = false; }
    // Back to the first cue at once, running.
    void restart(const CompositionSettings& s);

    // Advance by dt seconds: fades progress; in Timeline mode a cue whose hold is up
    // (and the show running) hands over to the next.
    void step(double dt, const CompositionSettings& s);

    // The events since the last call.
    CompositionEvents takeEvents();

private:
    void clampTo(const CompositionSettings& s);

    int current_ = 0;
    int previous_ = -1;
    double inCue_ = 0.0;
    double fadeElapsed_ = 0.0;
    bool running_ = true;
    CompositionEvents events_{0, {}};  // the first cue comes on at the start
};

class CompositionSource : public TextureSource {
public:
    explicit CompositionSource(CompositionSettings settings);
    ~CompositionSource() override;

    // --- TextureSource ---
    const UUID& getId() const override { return id_; }
    std::string getName() const override { return settings_.name; }
    SourceType getType() const override { return SourceType::COMPOSITION; }
    std::string getTypeName() const override { return "Composition"; }
    bool initialize() override { return true; }
    void shutdown() override;  // frees the GL objects (main thread, context current)
    bool isReady() const override { return true; }
    bool update(double) override { return false; }  // see advance() and render()
    bool hasNewFrame() const override { return false; }
    const FrameData* getCurrentFrame() const override { return nullptr; }
    void markFrameConsumed() override {}
    uint32_t getGLTextureId() const override;
    bool isGPUNative() const override { return true; }
    SourceCapabilities getCapabilities() const override;
    int getWidth() const override { return settings_.width; }
    int getHeight() const override { return settings_.height; }

    // --- The composition ---
    const CompositionSettings& settings() const { return settings_; }
    // Edit the settings (the cue list, timing, size). Takes effect next frame.
    CompositionSettings& editSettings() { return settings_; }
    CompositionPlayhead& playhead() { return playhead_; }
    // The first cue at once, its clip from the start (export, Restart).
    void restart() {
        playhead_.restart(settings_);
        primed_ = false;
    }
    const CompositionPlayhead& playhead() const { return playhead_; }

    // Once a frame, after the cues' sources updated: advance the playhead and start or
    // pause the cues' clips (restartOnCue).
    void advance(double dt, const std::function<TextureSource*(const UUID&)>& sourceOf);
    // Draw the current picture (the crossfade) into the composition's texture. Saves
    // and restores the GL state it touches.
    void render(const std::function<Texture*(const UUID&)>& textureOf);

private:
    bool ensureGl();

    UUID id_ = UUID::generate();
    CompositionSettings settings_;
    CompositionPlayhead playhead_;
    bool primed_ = false;  // the clips not on have been paused once
    bool shown_ = false;   // shownId_ is the cue last on (a change of the list moves it)
    UUID shownId_;
    std::unique_ptr<Framebuffer> frame_;
    std::unique_ptr<ShaderProgram> program_;
    uint32_t vao_ = 0;
    bool glFailed_ = false;  // the shader: for good
    int failedWidth_ = 0;    // the picture size that could not be allocated
    int failedHeight_ = 0;
};

}  // namespace quadmapper
