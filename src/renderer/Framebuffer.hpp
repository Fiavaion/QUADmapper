#pragma once

#include <glad/glad.h>

namespace quadmapper {

// RAII offscreen render target: an RGBA8 color texture backed by an FBO, used
// for rendering a scene to a texture instead of the screen (e.g. the per-output
// live previews, ADR-039 P4). By default a 24-bit depth renderbuffer lets 3D
// objects (ADR-052) occlude correctly; 2D quads are painter-ordered and ignore it,
// so a target only quads are drawn into (a scene canvas, ADR-047) is colour only.
//
// Create and destroy with the GL context that owns it current. The color texture
// is filled bottom-up (GL origin bottom-left), so when sampling for display flip
// V (uv0 = {0,1}, uv1 = {1,0}).
class Framebuffer {
public:
    enum class Depth { With, Without };

    explicit Framebuffer(Depth depth = Depth::With) : withDepth_(depth == Depth::With) {}
    ~Framebuffer();

    // Non-copyable (owns GL handles); not moved (held via unique_ptr where needed).
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    // Allocate the FBO + color texture, or give them a new size (same GL names, so
    // colorTexture() is unchanged by a resize). Returns true if the framebuffer is
    // complete and ready to use; false (and nothing allocated) when the GPU is out of
    // memory for it.
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
    GLuint depth_ = 0;  // renderbuffer; lives and dies with fbo_, so the FBO count covers it
    bool withDepth_ = true;
    int width_ = 0;
    int height_ = 0;
};

// Copies a Framebuffer's colour texture onto the draw framebuffer of ANOTHER context
// that shares it (ADR-054: an output window shows the frame the main context drew).
// Textures are shared between contexts, framebuffer objects are not, so each context
// that copies owns one of these. Create, use and destroy it with that context current.
class FrameCopy {
public:
    FrameCopy() = default;
    ~FrameCopy();

    FrameCopy(const FrameCopy&) = delete;
    FrameCopy& operator=(const FrameCopy&) = delete;

    // Copy `source` onto the bound draw framebuffer's rectangle (0,0)-(width,height):
    // texel for texel when the sizes match, filtered otherwise. The scissor test must
    // be off. False when the source has no texture or cannot be read.
    bool copy(const Framebuffer& source, int width, int height);

    // Release the GL object (call with the owning context current). Idempotent.
    void destroy();

private:
    GLuint fbo_ = 0;  // read framebuffer holding the source texture
};

}  // namespace quadmapper
