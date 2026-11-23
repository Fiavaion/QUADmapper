#include "CompositionSource.hpp"

#include "renderer/Framebuffer.hpp"  // glad first
#include "renderer/ShaderProgram.hpp"
#include "renderer/Texture.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cmath>

namespace quadmapper {

// ---- CompositionPlayhead --------------------------------------------------------

double CompositionPlayhead::fadeMix(const CompositionSettings& s) const {
    if (previous_ < 0 || !(s.fadeSeconds > 0.0)) return 1.0;
    return std::clamp(fadeElapsed_ / s.fadeSeconds, 0.0, 1.0);
}

void CompositionPlayhead::clampTo(const CompositionSettings& s) {
    const int n = static_cast<int>(s.cues.size());
    if (n == 0) {
        current_ = 0;
        previous_ = -1;
        return;
    }
    if (current_ >= n) current_ = n - 1;
    if (current_ < 0) current_ = 0;
    if (previous_ >= n || previous_ == current_) previous_ = -1;  // its cue is gone
}

void CompositionPlayhead::cue(int index, const CompositionSettings& s) {
    const int n = static_cast<int>(s.cues.size());
    if (n == 0) return;
    index = std::clamp(index, 0, n - 1);
    if (index != current_) {
        // A fade still running is cut short: that cue is out.
        if (previous_ >= 0 && previous_ != index) events_.faded.push_back(previous_);
        previous_ = current_;
    } else if (previous_ >= 0) {
        // The same cue again: the one fading out goes at once.
        events_.faded.push_back(previous_);
        previous_ = -1;
    }
    current_ = index;
    inCue_ = 0.0;
    fadeElapsed_ = 0.0;
    events_.started = index;
    if (!(s.fadeSeconds > 0.0) && previous_ >= 0) {
        events_.faded.push_back(previous_);
        previous_ = -1;
    }
}

void CompositionPlayhead::next(const CompositionSettings& s) {
    const int n = static_cast<int>(s.cues.size());
    if (n == 0) return;
    cue((current_ + 1) % n, s);
}

void CompositionPlayhead::prev(const CompositionSettings& s) {
    const int n = static_cast<int>(s.cues.size());
    if (n == 0) return;
    cue((current_ + n - 1) % n, s);
}

void CompositionPlayhead::restart(const CompositionSettings& s) {
    current_ = 0;
    previous_ = -1;
    inCue_ = 0.0;
    fadeElapsed_ = 0.0;
    running_ = true;
    events_ = CompositionEvents{};
    if (!s.cues.empty()) events_.started = 0;
}

void CompositionPlayhead::step(double dt, const CompositionSettings& s) {
    clampTo(s);
    const int n = static_cast<int>(s.cues.size());
    if (n == 0 || !(dt >= 0.0)) return;
    fadeElapsed_ += dt;
    if (previous_ >= 0 && fadeElapsed_ >= s.fadeSeconds) {
        events_.faded.push_back(previous_);
        previous_ = -1;
    }
    if (!running_ || s.mode != CompositionMode::Timeline) return;
    inCue_ += dt;
    // A hold of 0 (or less) waits for a command.
    for (int guard = 0; guard < n; ++guard) {
        const double hold = s.cues[static_cast<size_t>(current_)].holdSeconds;
        if (!(hold > 0.0) || inCue_ < hold) return;
        if (current_ == n - 1 && !s.loop) {
            running_ = false;  // holds on the last cue
            return;
        }
        const double over = inCue_ - hold;
        cue((current_ + 1) % n, s);
        inCue_ = over;
        fadeElapsed_ = over;
        if (previous_ >= 0 && fadeElapsed_ >= s.fadeSeconds) {
            events_.faded.push_back(previous_);
            previous_ = -1;
        }
    }
}

CompositionEvents CompositionPlayhead::takeEvents() {
    const CompositionEvents events = events_;
    events_ = CompositionEvents{};
    return events;
}

// ---- CompositionSource ----------------------------------------------------------

namespace {

// One triangle covering the target; output row r samples picture row r, the
// orientation of every other source texture (as YuvConverter).
constexpr const char* VERTEX_SOURCE = R"GLSL(
#version 330 core
out vec2 vUv;
void main() {
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// The cue fading out (A) under the one fading in (B), by uMix.
constexpr const char* FRAGMENT_SOURCE = R"GLSL(
#version 330 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uA;
uniform sampler2D uB;
uniform bool uHasA;
uniform bool uHasB;
uniform float uMix;
void main() {
    vec3 a = uHasA ? texture(uA, vUv).rgb : vec3(0.0);
    vec3 b = uHasB ? texture(uB, vUv).rgb : vec3(0.0);
    fragColor = vec4(mix(a, b, uMix), 1.0);
}
)GLSL";

}  // namespace

CompositionSource::CompositionSource(CompositionSettings settings) : settings_(std::move(settings)) {
    settings_.width = std::max(2, settings_.width);
    settings_.height = std::max(2, settings_.height);
}

CompositionSource::~CompositionSource() { shutdown(); }

void CompositionSource::shutdown() {
    frame_.reset();
    if (program_) program_->destroy();
    program_.reset();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
}

uint32_t CompositionSource::getGLTextureId() const {
    return frame_ && frame_->valid() ? frame_->colorTexture() : 0;
}

SourceCapabilities CompositionSource::getCapabilities() const {
    SourceCapabilities caps;
    caps.isStatic = false;
    caps.hasPlayback = false;
    caps.hasParameters = true;
    caps.supportsAsync = false;
    caps.isGPUNative = true;
    return caps;
}

void CompositionSource::advance(double dt, const std::function<TextureSource*(const UUID&)>& sourceOf) {
    const auto cueSource = [&](int index) -> PlayableSource* {
        if (index < 0 || index >= static_cast<int>(settings_.cues.size())) return nullptr;
        return dynamic_cast<PlayableSource*>(sourceOf(settings_.cues[static_cast<size_t>(index)].sourceId));
    };
    // A video added before its file opened holds for the clip's length once known.
    for (size_t i = 0; i < settings_.cues.size(); ++i) {
        CompositionCue& cue = settings_.cues[i];
        if (!cue.holdFromClip) continue;
        PlayableSource* clip = cueSource(static_cast<int>(i));
        if (!clip) {
            cue.holdFromClip = false;  // gone, or not a clip
        } else if (clip->getDuration() > 0.0) {
            cue.holdSeconds = clip->getDuration();
            cue.holdFromClip = false;
        }
    }

    playhead_.step(dt, settings_);
    CompositionEvents events = playhead_.takeEvents();
    const auto currentCueId = [&] {
        return settings_.cues.empty() ? UUID() : settings_.cues[static_cast<size_t>(playhead_.current())].sourceId;
    };
    // The cue list changed under the cue showing (it was removed, or one before it):
    // the cue now in its place comes on, from its start.
    if (events.started < 0 && shown_ && currentCueId() != shownId_ && !settings_.cues.empty()) {
        playhead_.cue(playhead_.current(), settings_);
        CompositionEvents more = playhead_.takeEvents();
        events.started = more.started;
        events.faded.insert(events.faded.end(), more.faded.begin(), more.faded.end());
    }
    const UUID currentId = currentCueId();
    shown_ = true;
    shownId_ = currentId;

    if (!settings_.restartOnCue) {
        // Turned off: the clips it paused run again ("every clip keeps running").
        if (primed_) {
            for (size_t i = 0; i < settings_.cues.size(); ++i) {
                if (PlayableSource* clip = cueSource(static_cast<int>(i))) clip->play();
            }
        }
        primed_ = false;
        return;
    }
    const UUID previousId = playhead_.previous() >= 0
                                ? settings_.cues[static_cast<size_t>(playhead_.previous())].sourceId
                                : UUID();
    // Once: every clip that is not on starts paused.
    if (!primed_) {
        primed_ = true;
        for (size_t i = 0; i < settings_.cues.size(); ++i) {
            const UUID& id = settings_.cues[i].sourceId;
            if (id == currentId || id == previousId) continue;
            if (PlayableSource* clip = cueSource(static_cast<int>(i))) clip->pause();
        }
    }
    if (events.started >= 0) {
        if (PlayableSource* clip = cueSource(events.started)) {
            if (clip->canSeek()) clip->seek(0.0);
            clip->play();
        }
    }
    for (int faded : events.faded) {
        if (faded < 0 || faded >= static_cast<int>(settings_.cues.size())) continue;
        const UUID& fadedId = settings_.cues[static_cast<size_t>(faded)].sourceId;
        if (fadedId != currentId && fadedId != previousId) {
            if (PlayableSource* clip = cueSource(faded)) clip->pause();
        }
    }
}

bool CompositionSource::ensureGl() {
    if (glFailed_) return false;
    if (!program_) {
        program_ = std::make_unique<ShaderProgram>();
        if (!program_->create(VERTEX_SOURCE, FRAGMENT_SOURCE)) {
            Logger::error("Composition '" + settings_.name + "': the blend shader did not compile");
            glFailed_ = true;
            return false;
        }
        glGenVertexArrays(1, &vao_);  // core profile: a draw needs a bound VAO
    }
    if (!frame_) frame_ = std::make_unique<Framebuffer>(Framebuffer::Depth::Without);
    if (frame_->width() != settings_.width || frame_->height() != settings_.height || !frame_->valid()) {
        // A size that failed is not retried every frame; another size is.
        if (settings_.width == failedWidth_ && settings_.height == failedHeight_) return false;
        if (!frame_->resize(settings_.width, settings_.height)) {
            Logger::error("Composition '" + settings_.name + "': could not allocate its picture");
            failedWidth_ = settings_.width;
            failedHeight_ = settings_.height;
            return false;
        }
        failedWidth_ = failedHeight_ = 0;
    }
    return true;
}

void CompositionSource::render(const std::function<Texture*(const UUID&)>& textureOf) {
    if (!ensureGl()) return;

    const auto textureFor = [&](int index) -> GLuint {
        if (index < 0 || index >= static_cast<int>(settings_.cues.size())) return 0;
        const UUID& id = settings_.cues[static_cast<size_t>(index)].sourceId;
        if (id.isNull()) return 0;
        Texture* texture = textureOf(id);
        return texture && texture->isValid() ? static_cast<GLuint>(texture->getId()) : 0;
    };
    const GLuint b = settings_.cues.empty() ? 0 : textureFor(playhead_.current());
    const GLuint a = textureFor(playhead_.previous());
    const float mix = playhead_.previous() >= 0 ? static_cast<float>(playhead_.fadeMix(settings_)) : 1.0f;

    GLint drawFbo = 0, readFbo = 0, program = 0, vao = 0, activeTexture = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint bound0 = 0, bound1 = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    const bool blend = glIsEnabled(GL_BLEND) == GL_TRUE;
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    const bool cull = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    const bool depth = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound1);

    frame_->bind();
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    program_->bind();
    program_->setInt("uA", 0);
    program_->setInt("uB", 1);
    program_->setInt("uHasA", a != 0 ? 1 : 0);
    program_->setInt("uHasB", b != 0 ? 1 : 0);
    program_->setFloat("uMix", mix);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, b);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound1));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound0));
    glActiveTexture(static_cast<GLenum>(activeTexture));
    glBindVertexArray(static_cast<GLuint>(vao));
    glUseProgram(static_cast<GLuint>(program));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    const auto setEnabled = [](GLenum cap, bool on) { on ? glEnable(cap) : glDisable(cap); };
    setEnabled(GL_BLEND, blend);
    setEnabled(GL_SCISSOR_TEST, scissor);
    setEnabled(GL_CULL_FACE, cull);
    setEnabled(GL_DEPTH_TEST, depth);
}

}  // namespace quadmapper
