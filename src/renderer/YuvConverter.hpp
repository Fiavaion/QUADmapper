#pragma once

#include "ShaderProgram.hpp"
#include "core/YuvPicture.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace quadmapper {

class Texture;
class PlaneCopier;

/**
 * @brief GL objects one video source needs to upload its YUV planes (ADR-055)
 *
 * Three plane textures (R8/RG8, or R16/RG16 for 10-bit; NV12 uses two) and a small ring
 * of pixel buffer objects, so glTexSubImage2D reads from a buffer. The GL names exist
 * for the state's whole life (their storage is sized at the first frame), so a video
 * source holds a fixed number of GL objects. Main thread, main GL context only.
 */
class YuvUploadState {
public:
    YuvUploadState();   // generates the GL names: needs the GL context
    ~YuvUploadState();  // waits for a copy still writing into a buffer, then unmaps it
    YuvUploadState(const YuvUploadState&) = delete;
    YuvUploadState& operator=(const YuvUploadState&) = delete;

    // Whether the destructor would free it at once: true once no copy writes into its
    // buffers (a copy still queued is dropped, it never starts), false while one runs.
    // Never waits (TD-35: a removed source's state waited on the main thread for its
    // copy). Main thread.
    bool idle();

    // A frame being copied into one of the pixel buffers, which stays mapped until the
    // upload. Written by the copier thread only between submit and done.
    struct Staging {
        YuvPicture picture;          // the frame: its handle keeps the planes alive
        uint8_t* mapped = nullptr;   // the buffer's mapping; null when not mapped
        int pbo = 0;                 // which buffer
        size_t offsets[3] = {0, 0, 0};
        bool done = true;            // guarded by the copier's mutex
    };

private:
    friend class YuvConverter;
    static constexpr int PBO_COUNT = 2;

    // Wait for the copy and unmap: before the buffers' storage changes, and at the end.
    void settle();

    Staging staging_;
    std::shared_ptr<PlaneCopier> copier_;  // the converter's copier, once used
    YuvLayout layout_ = YuvLayout::None;
    int width_ = 0;
    int height_ = 0;
    uint32_t planes_[3] = {0, 0, 0};
    uint32_t pbos_[PBO_COUNT] = {0, 0};
    size_t pboBytes_ = 0;
    int nextPbo_ = 0;
};

/**
 * @brief Converts YUV pictures into a source's RGBA texture on the GPU (ADR-055)
 *
 * Uploads the planes through the source's YuvUploadState, then draws one full-screen
 * triangle into the RGBA texture with the picture's matrix (BT.601/709/2020) and range
 * (limited/full). Every consumer keeps sampling that one RGBA texture, so the quad and
 * mesh shaders are unchanged. One converter per GL context (it owns an FBO and a VAO,
 * which contexts do not share); main thread only.
 *
 * The frame's bytes reach the pixel buffer on the converter's copier thread, never on
 * the main thread: the main thread maps a buffer and the copier fills it (a mapped
 * buffer's memory is ordinary memory; only map, unmap and the GL calls stay on the main
 * thread). prefetch() starts the copy of the frame expected next, so by its upload the
 * bytes are usually in place and the main thread only unmaps and issues the upload.
 *
 * Call begin() before a batch of convert() calls and end() after: the GL state the pass
 * changes is saved once and restored once, not per frame converted.
 */
class YuvConverter {
public:
    YuvConverter() = default;
    ~YuvConverter();
    YuvConverter(const YuvConverter&) = delete;
    YuvConverter& operator=(const YuvConverter&) = delete;

    // Build the shader and GL objects now instead of at the first frame (TD-35). GL context current.
    bool prepare();
    void begin();
    // Upload `picture` and write it into `target`, which must be an RGBA texture of the
    // picture's size (the caller allocates it). Returns false if the GL objects could
    // not be created (the target is left unchanged).
    bool convert(const YuvPicture& picture, Texture& target, YuvUploadState& state);
    // Start copying `next`, the frame expected after the one just converted, into a
    // spare buffer. convert() uses it if that frame comes next, else copies anew.
    void prefetch(const YuvPicture& next, YuvUploadState& state);
    // Convert a semi-planar (NV12/P010) picture whose planes are already GL textures
    // (a zero-copy frame shared from D3D11, ADR-056): `luma` R8/R16, `chroma` RG8/RG16,
    // as the upload path would fill them. `picture` gives the layout, size, matrix and
    // range. False if the GL objects could not be created or the arguments do not fit.
    bool convertPlanes(const YuvPicture& picture, uint32_t luma, uint32_t chroma, Texture& target);
    void end();

    /**
     * @brief rgb = matrix * (scale * sample - offset), per the picture's matrix and range
     *
     * sample is the texture value (0-1 of the container: 8 or 16 bits); scale turns
     * 10-bit samples stored in 16 bits into 0-1 of the 10-bit range. Exposed for tests.
     */
    struct Coefficients {
        glm::mat3 matrix{1.0f};
        glm::vec3 offset{0.0f};
        float scale = 1.0f;
    };
    static Coefficients coefficients(YuvMatrix matrix, bool fullRange, YuvLayout layout);

    // Test hook: run on the copier thread before each frame copy (every converter); it
    // may block. Set it while no copy is queued.
    static void setCopyHookForTesting(std::function<void()> hook);

private:
    bool ensureObjects();
    bool ensurePlanes(const YuvPicture& picture, YuvUploadState& state);
    bool uploadPlanes(const YuvPicture& picture, YuvUploadState& state);
    bool mapStaging(YuvUploadState& state);
    void stageCopy(const YuvPicture& picture, YuvUploadState& state);
    // The conversion draw from the plane textures into `target`, then its mipmaps.
    void drawPass(const YuvPicture& picture, const uint32_t planes[3], Texture& target);
    void saveState();
    void restoreState();

    ShaderProgram program_;
    std::shared_ptr<PlaneCopier> copier_;
    uint32_t vao_ = 0;
    uint32_t fbo_ = 0;
    bool failed_ = false;  // shader or FBO creation failed: don't retry every frame

    // GL state saved by begin() on the first convert() and restored by end().
    bool inBatch_ = false;
    bool saved_ = false;
    int savedDrawFbo_ = 0;
    int savedReadFbo_ = 0;
    int savedViewport_[4] = {0, 0, 0, 0};
    int savedProgram_ = 0;
    int savedVao_ = 0;
    int savedActiveTexture_ = 0;
    int savedUnpackBuffer_ = 0;
    int savedUnpackAlignment_ = 4;
    int savedUnpackRowLength_ = 0;
    bool savedBlend_ = false;
    bool savedScissor_ = false;
    bool savedCull_ = false;
    bool savedDepth_ = false;
};

}  // namespace quadmapper
