#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace quadmapper {

class Texture;
class ByteCopier;

/**
 * @brief GL objects one GPU-compressed (HAP) source uploads through (ADR-044 amendment)
 *
 * Two pixel buffer objects, so glCompressedTexSubImage2D reads from a buffer instead of
 * copying the frame out of client memory on the main thread. The names exist for the
 * state's whole life, so a source holds a fixed number of GL objects. Main thread, main
 * GL context only.
 */
class CompressedUploadState {
public:
    CompressedUploadState();   // generates the GL names: needs the GL context
    ~CompressedUploadState();  // waits for a copy still writing into a buffer, then unmaps it
    CompressedUploadState(const CompressedUploadState&) = delete;
    CompressedUploadState& operator=(const CompressedUploadState&) = delete;

    // Whether the destructor would free it at once: true once no copy writes into its
    // buffers (a copy still queued is dropped, it never starts), false while one runs.
    // Never waits (TD-35). Main thread.
    bool idle();

    // A frame being copied into one of the pixel buffers, which stays mapped until the
    // upload. Written by the copier thread only between submit and done.
    struct Staging {
        std::shared_ptr<const std::vector<uint8_t>> bytes;  // the frame; held until uploaded
        uint8_t* mapped = nullptr;  // the buffer's mapping; null when not mapped
        size_t mappedBytes = 0;     // the mapping's size
        int pbo = 0;                // which buffer
        bool done = true;           // guarded by the copier's mutex
    };

private:
    friend class CompressedUploader;
    static constexpr int PBO_COUNT = 2;

    // Wait for the copy and unmap: before the buffer is used another way, and at the end.
    void settle();

    Staging staging_;
    std::shared_ptr<ByteCopier> copier_;  // the uploader's copier, once used
    uint32_t pbos_[PBO_COUNT] = {0, 0};
    int nextPbo_ = 0;
};

/**
 * @brief Uploads BCn frames into compressed textures through pixel buffers (TD-31)
 *
 * The frame's bytes reach the pixel buffer on the uploader's copier thread, never on
 * the main thread: the main thread maps a buffer and the copier fills it (a mapped
 * buffer's memory is ordinary memory; map, unmap and the GL calls stay on the main
 * thread), as YuvConverter does for YUV planes (ADR-055). prefetch() starts the copy of
 * the frame expected next, so by its upload the bytes are usually in place and the main
 * thread only unmaps and issues the upload, which then returns at once. Main thread only.
 */
class CompressedUploader {
public:
    CompressedUploader() = default;
    ~CompressedUploader() = default;
    CompressedUploader(const CompressedUploader&) = delete;
    CompressedUploader& operator=(const CompressedUploader&) = delete;

    // Upload `bytes` as the whole level of `target`, a compressed texture of the frame's
    // format and size. False if nothing could be uploaded (no bytes, or a pixel buffer
    // could not be mapped). Leaves no pixel unpack buffer bound.
    bool upload(const std::shared_ptr<const std::vector<uint8_t>>& bytes, Texture& target,
                CompressedUploadState& state);
    // Start copying `next`, the frame expected after the one just uploaded, into a spare
    // buffer. upload() uses it if that frame comes next, else copies anew.
    void prefetch(const std::shared_ptr<const std::vector<uint8_t>>& next, CompressedUploadState& state);

    // Test hook: run on the copier thread before each frame copy (every uploader); it
    // may block. Set it while no copy is queued.
    static void setCopyHookForTesting(std::function<void()> hook);

private:
    bool mapStaging(CompressedUploadState& state, size_t bytes);
    void stageCopy(const std::shared_ptr<const std::vector<uint8_t>>& bytes, CompressedUploadState& state);

    std::shared_ptr<ByteCopier> copier_;
};

}  // namespace quadmapper
