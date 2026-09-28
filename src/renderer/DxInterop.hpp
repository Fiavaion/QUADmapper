#pragma once

#include "core/YuvPicture.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace quadmapper {

class Texture;
class YuvConverter;
struct InteropDevice;

/**
 * @brief One video source's YUV planes shared between D3D11 and OpenGL (ADR-056)
 *
 * Two D3D11 textures, luma (R8, or R16 for 10-bit) and chroma (RG8 or RG16), that each
 * frame's planes are copied into on the GPU, registered once with OpenGL through
 * WGL_NV_DX_interop2. Created on the first frame, re-created if the frame size or format
 * changes. The destructor unregisters the textures from OpenGL before either side's
 * texture is released, so it must run with the GL context (or one sharing with it)
 * current. Main thread only.
 */
class DxInteropTarget {
public:
    DxInteropTarget();
    ~DxInteropTarget();
    DxInteropTarget(const DxInteropTarget&) = delete;
    DxInteropTarget& operator=(const DxInteropTarget&) = delete;

private:
    friend class DxInterop;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Zero-copy video frames: D3D11 surfaces into OpenGL textures (ADR-056)
 *
 * Hardware-decoded frames stay on the GPU (YuvPicture::gpu), in textures the decoders
 * share with a keyed mutex. DxInterop has its own D3D11 device on the same adapter, used
 * by the main thread only, so it never waits on the decoders' device. Each update,
 * stage() queues a source's new frame and finish() delivers them all into the sources'
 * own RGBA GL textures, the textures every consumer samples:
 *  1. a compute pass copies every frame's NV12/P010 planes (opened on this device,
 *     under the frame's keyed mutex) into its source's shared plane textures, samples
 *     unchanged; the context is flushed and all the shared textures are locked for
 *     OpenGL in one call;
 *  2. YuvConverter converts each source's shared planes into its RGBA texture: the same
 *     pass, matrix and range as frames uploaded from memory, so both paths give the same
 *     pixels;
 *  3. the shared textures are unlocked.
 * If the one lock fails, each source's planes are locked on their own, so only the
 * sources whose own lock fails are reported. The interop lock is held only inside
 * finish(): the shared textures are never sampled elsewhere, so the render passes are
 * untouched.
 *
 * A source that cannot use this (extension missing, OpenGL on another GPU than the
 * decoders, registration or format refused, a run-time interop error) is reported to
 * the caller, which carries on through the readback path. One per GL context; main
 * thread only.
 */
class DxInterop {
public:
    // Force each fallback in tests. Set before the first frame is staged.
    struct TestHooks {
        bool extensionMissing = false;  // as if the driver lacked WGL_NV_DX_interop2
        bool failRegistration = false;  // wglDXRegisterObjectNV fails
        bool rejectFormat = false;      // the frame's pixel format cannot be shared
        int failLockAfter = -1;         // every wglDXLockObjectsNV fails after this many batches
        const Texture* failLockDest = nullptr;  // any lock including the planes for this texture fails
        bool adapterMismatch = false;   // as if OpenGL ran on another GPU than the decoders
    };
    static TestHooks& testHooks();

    // D3D11 objects and interop registrations held for sources (not the interop device
    // itself), for leak checks: 0 once every video source is gone.
    static long liveObjects();
    // Interop devices open (at most one per DxInterop, i.e. per SourceManager; it
    // lives until that is destroyed), for leak checks.
    static long liveDevices();

    // The LUID of the adapter the current GL context renders on, from
    // GL_EXT_memory_object_win32 / GL_EXT_semaphore_win32; false, with the reason in
    // `why`, when the driver does not say. Needs a current GL context.
    static bool glAdapterLuid(uint64_t& luid, std::string& why);

    DxInterop();   // no GL or D3D work until the first frame
    ~DxInterop();  // closes the interop device once no target holds it: GL context current
    DxInterop(const DxInterop&) = delete;
    DxInterop& operator=(const DxInterop&) = delete;

    /**
     * @brief Whether the current GL context can share D3D11 textures
     *
     * False, with unavailableReason(), when the extension is missing or opening the
     * D3D11 device for sharing failed once. Main thread, GL context current.
     */
    bool available();
    const std::string& unavailableReason() const { return unavailableReason_; }

    /**
     * @brief Queue a GPU frame for `dest`, through `target`'s shared planes
     * @param key Handed back to finish()'s failure callback
     * @param why Set when the frame cannot take this path (the source should fall back)
     * @return false if the source cannot use zero-copy; nothing was queued
     */
    bool stage(const YuvPicture& picture, DxInteropTarget& target, Texture& dest, void* key,
               std::string& why);

    using Failure = std::function<void(void* key, const YuvPicture& picture, const std::string& why)>;

    /**
     * @brief Deliver every staged frame into its texture (see the class comment)
     *
     * `converter` does the GL conversion inside its begin()/end() batch. Frames that
     * failed are passed to onFailure, which should fall back for that source.
     */
    void finish(YuvConverter& converter, const Failure& onFailure);

private:
    struct Pending {
        YuvPicture picture;
        DxInteropTarget* target = nullptr;
        Texture* dest = nullptr;
        void* key = nullptr;
    };

    bool openDevice(const GpuSurface& surface, std::string& why);

    std::shared_ptr<InteropDevice> device_;
    std::vector<Pending> pending_;
    // finish()'s working lists, kept so a frame allocates nothing.
    std::vector<Pending> delivering_;
    std::vector<void*> handles_;  // HANDLEs of the shared planes to lock
    std::vector<size_t> copied_;
    std::vector<std::pair<size_t, std::string>> failed_;
    bool checked_ = false;
    bool available_ = false;
    std::string unavailableReason_;
    int batches_ = 0;
};

}  // namespace quadmapper
