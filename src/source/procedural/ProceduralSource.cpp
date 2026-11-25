#include "ProceduralSource.hpp"
#include "core/Logger.hpp"
#include <glad/glad.h>

#include <algorithm>

namespace quadmapper {

namespace {

// One triangle that covers the viewport; no vertex buffer.
const char* const FULLSCREEN_VERTEX_SHADER = R"(
#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

}  // namespace

ProceduralSource::ProceduralSource(const std::string& name, int width, int height)
    : name_(name)
    , width_(width)
    , height_(height)
{
}

ProceduralSource::~ProceduralSource() {
    shutdown();
}

bool ProceduralSource::initialize() {
    if (ready_) {
        return true;
    }

    createOutputTexture();
    if (outputTexture_ == 0) {
        Logger::error("ProceduralSource '{}': Failed to create output texture", name_);
        return false;
    }

    // Compile the fragment shader
    shader_ = std::make_unique<ShaderProgram>();
    std::string source = getShaderSource();
    if (!shader_->create(FULLSCREEN_VERTEX_SHADER, source.c_str())) {
        Logger::error("ProceduralSource '{}': Failed to compile shader", name_);
        shader_.reset();
        destroyOutputTexture();
        return false;
    }

    // The output texture is the framebuffer's only attachment. Only the draw binding is
    // used, and it is left as it was: the caller's read binding is never touched.
    GLint previousFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFbo);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture_, 0);
    const bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
    if (!complete) {
        Logger::error("ProceduralSource '{}': Output framebuffer is incomplete", name_);
        shutdown();
        return false;
    }
    glGenVertexArrays(1, &vao_);

    ready_ = true;
    dirty_ = true;

    Logger::debug("ProceduralSource '{}': Initialized ({}x{})", name_, width_, height_);
    return true;
}

void ProceduralSource::shutdown() {
    shader_.reset();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    destroyOutputTexture();
    ready_ = false;
    dirty_ = true;
}

bool ProceduralSource::update(double deltaTime) {
    if (!ready_) {
        return false;
    }

    // Accumulate time for animations
    time_ += deltaTime;

    // Let subclass decide if it needs to regenerate
    bool needsUpdate = onUpdate(deltaTime);
    if (needsUpdate) {
        dirty_ = true;
    }

    // Regenerate if dirty
    if (dirty_) {
        regenerate();
        return true;
    }

    return false;
}

SourceCapabilities ProceduralSource::getCapabilities() const {
    SourceCapabilities caps;
    caps.isStatic = !isAnimated();
    caps.hasPlayback = false;
    caps.hasParameters = true;
    caps.supportsAsync = false;  // GPU work is synchronous on main thread
    caps.isGPUNative = true;
    return caps;
}

void ProceduralSource::regenerate() {
    if (!ready_ || !shader_ || fbo_ == 0 || outputTexture_ == 0) {
        return;
    }

    // Patterns render in the middle of the source update pass, so everything touched
    // here is put back afterwards (as CompositionSource does).
    GLint drawFbo = 0, program = 0, vao = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_VIEWPORT, viewport);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean cull = glIsEnabled(GL_CULL_FACE);
    GLboolean colorMask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    shader_->bind();
    shader_->setVec2("u_Resolution", Vec2(static_cast<float>(width_), static_cast<float>(height_)));
    shader_->setFloat("u_Time", static_cast<float>(time_));

    // Let subclass set its uniforms
    setShaderUniforms();

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(static_cast<GLuint>(vao));
    glUseProgram(static_cast<GLuint>(program));
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

    dirty_ = false;
}

void ProceduralSource::createOutputTexture() {
    if (outputTexture_ != 0) {
        return;
    }

    constexpr int MAX_SIDE = 8192;
    width_ = std::clamp(width_, 1, MAX_SIDE);
    height_ = std::clamp(height_, 1, MAX_SIDE);

    glGenTextures(1, &outputTexture_);
    glBindTexture(GL_TEXTURE_2D, outputTexture_);

    // Allocate storage (RGBA8, the framebuffer's colour attachment)
    while (glGetError() != GL_NO_ERROR) {}
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glBindTexture(GL_TEXTURE_2D, 0);
        destroyOutputTexture();
        return;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, 0);
}

void ProceduralSource::destroyOutputTexture() {
    if (outputTexture_ != 0) {
        glDeleteTextures(1, &outputTexture_);
        outputTexture_ = 0;
    }
}

} // namespace quadmapper
