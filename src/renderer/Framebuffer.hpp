#pragma once

#include <glad/glad.h>

namespace quadmapper {

// RAII offscreen render target: an RGBA8 color texture backed by an FBO, used
// for rendering a scene to a texture instead of the screen (e.g. the per-output
// live previews, ADR-039 P4). Color-only — 2D quad rendering is painter-ordered
// and needs no depth buffer.
//
// Create and destroy with the GL context that owns it current. The color texture
// is filled bottom-up (GL origin bottom-left), so when sampling for display flip
// V (uv0 = {0,1}, uv1 = {1,0}).
class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();

    // Non-copyable (owns GL handles); not moved (held via unique_ptr where needed).
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    // Allocate (or reallocate, if the size changed) the FBO + color texture.
    // Returns true if the framebuffer is complete and ready to use.
    bool resize(int width, int height);

    // Bind for drawing and set the GL viewport to the framebuffer's size.
    void bind() const;

    // Bind the default framebuffer (0). The caller is responsible for restoring
    // its own viewport afterwards.
    static void unbind();

    // Release the GL objects (call with the owning context current). Idempotent.
    void destroy();

    GLuint colorTexture() const { return color_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool valid() const { return fbo_ != 0; }

private:
    GLuint fbo_ = 0;
    GLuint color_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace quadmapper
