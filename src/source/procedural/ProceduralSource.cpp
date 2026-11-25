#include "ProceduralSource.hpp"
#include "core/Logger.hpp"
#include <glad/glad.h>

#include <algorithm>

namespace quadmapper {

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

    // Compile compute shader
    shader_ = std::make_unique<ComputeShader>();
    std::string source = getShaderSource();
    if (!shader_->loadFromSource(source)) {
        Logger::error("ProceduralSource '{}': Failed to compile shader: {}",
                      name_, shader_->getErrorMessage());
        destroyOutputTexture();
        return false;
    }

    ready_ = true;
    dirty_ = true;

    Logger::debug("ProceduralSource '{}': Initialized ({}x{})", name_, width_, height_);
    return true;
}

void ProceduralSource::shutdown() {
    shader_.reset();
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
    if (!ready_ || !shader_ || outputTexture_ == 0) {
        return;
    }

    shader_->bind();

    shader_->setUniform("u_Resolution", Vec2(static_cast<float>(width_), static_cast<float>(height_)));
    shader_->setUniform("u_Time", static_cast<float>(time_));

    // Let subclass set its uniforms
    setShaderUniforms();

    shader_->bindImageTexture(0, outputTexture_, GL_WRITE_ONLY, GL_RGBA8);

    // Calculate work groups (16x16 local size is common)
    uint32_t groupsX = (width_ + 15) / 16;
    uint32_t groupsY = (height_ + 15) / 16;

    // Dispatch compute shader
    shader_->dispatch(groupsX, groupsY, 1);

    // Wait for completion
    shader_->waitForCompletion();

    // Unbind
    ComputeShader::unbind();

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

    // Allocate storage (RGBA8 for compute shader output)
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
