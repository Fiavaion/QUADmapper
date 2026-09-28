#include "Framebuffer.hpp"
#include "core/GlObjectStats.hpp"

namespace quadmapper {

Framebuffer::~Framebuffer() {
    destroy();
}

bool Framebuffer::resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (fbo_ != 0 && width == width_ && height == height_) {
        return true;  // already the right size
    }

    const bool created = fbo_ == 0;
    if (created) {
        glGenTextures(1, &color_);
        GlObjectStats::onTextureCreated();
        if (withDepth_) glGenRenderbuffers(1, &depth_);
        glGenFramebuffers(1, &fbo_);
        GlObjectStats::onFramebufferCreated();
    }
    width_ = width;
    height_ = height;

    // A new size re-specifies the storage of the same objects: the colour texture
    // keeps its name, so a draw already recorded with it (an ImGui::Image of an
    // output frame) samples the resized texture instead of a deleted one.
    // Earlier errors are not this allocation's; clear them so an out-of-memory
    // below (which leaves the old storage in place) is seen. Bounded: a lost context
    // can report an error on every call.
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    if (withDepth_) {
        glBindRenderbuffer(GL_RENDERBUFFER, depth_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width_, height_);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    const bool allocated = glGetError() != GL_OUT_OF_MEMORY;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    if (created) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               color_, 0);
        if (withDepth_) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
        }
    }

    const bool complete =
        allocated && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!complete) {
        destroy();
        return false;
    }
    return true;
}

void Framebuffer::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
}

void Framebuffer::unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Framebuffer::destroy() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        GlObjectStats::onFramebufferDestroyed();
        fbo_ = 0;
    }
    if (depth_ != 0) {
        glDeleteRenderbuffers(1, &depth_);
        depth_ = 0;
    }
    if (color_ != 0) {
        glDeleteTextures(1, &color_);
        GlObjectStats::onTextureDestroyed();
        color_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

FrameCopy::~FrameCopy() {
    destroy();
}

bool FrameCopy::copy(const Framebuffer& source, int width, int height) {
    if (!source.valid() || width <= 0 || height <= 0) {
        return false;
    }
    if (fbo_ == 0) {
        glGenFramebuffers(1, &fbo_);
        GlObjectStats::onFramebufferCreated();
    }

    GLint previousRead = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    // Attached on every copy: the texture was drawn in another context (and may have
    // been reallocated there), and attaching it here is what makes those contents
    // visible in this one (GL 3.3 core, appendix D.3.3).
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           source.colorTexture(), 0);
    const bool readable =
        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (readable) {
        const bool sameSize = width == source.width() && height == source.height();
        glBlitFramebuffer(0, 0, source.width(), source.height(), 0, 0, width, height,
                          GL_COLOR_BUFFER_BIT, sameSize ? GL_NEAREST : GL_LINEAR);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    return readable;
}

void FrameCopy::destroy() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        GlObjectStats::onFramebufferDestroyed();
        fbo_ = 0;
    }
}

}  // namespace quadmapper
