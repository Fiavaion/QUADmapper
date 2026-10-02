#pragma once

#include "core/YuvPicture.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace quadmapper {

class BackgroundWorker;
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

    // Free it a piece at a time (TD-35: unregistering a shared plane took up to 53 ms
    // under load): each call releases one shared plane (its GL registration, GL name and
    // D3D11 texture), the last one also the decoded frames it had opened. True once
    // nothing is left. GL context current; main thread.
    bool releaseStep();

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
        int spareLifetimeMs = -1;       // in place of SPARE_LIFETIME when >= 0
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
     * @brief Create the interop D3D11 device ahead of the first frame (TD-35)
     * @param decoderDevice The decoders' ID3D11Device (VideoDecoder::sharedD3dDevice())
     *
     * The D3D11 device on the decoders' adapter and its plane copy shader, the slow part
     * of the first zero-copy frame (up to 3.1 s under load, on the main thread). The next
     * beginFrame() (or the first stage()) then only opens it for OpenGL, which needs the
     * GL context. Any thread;
     * does nothing once a device is prepared or open, or if `decoderDevice` is null. A
     * failure here is not final: stage() tries again on the main thread and reports it.
     */
    void prepare(void* decoderDevice);

    /**
     * @brief Queue a GPU frame for `dest`, through `target`'s shared planes
     * @param key Handed back to finish()'s failure callback
     * @param why Set when the frame cannot take this path (the source should fall back)
     * @return false if the source cannot use zero-copy; nothing was queued
     */
    bool stage(const YuvPicture& picture, DxInteropTarget& target, Texture& dest, void* key,
               std::string& why);

    /**
     * @brief Start a frame's allowance of zero-copy set-up (TD-35)
     *
     * Call once per update, before setUp(). The allowance is one costly GL call a frame.
     * When prepare() has made the device and it is not open yet, it is opened for OpenGL
     * here (wglDXOpenDeviceNV), spending the allowance, so the first frame of the first
     * stream on the GPU does not open it.
     */
    void beginFrame();

    enum class SetUp {
        Ready,   // stage() will only queue the frame
        Later,   // steps remain: keep the frame, ask again next update
        Failed,  // the source cannot use zero-copy (`why` says why)
    };

    /**
     * @brief Set up `target` for frames like `picture` a step at a time (TD-35)
     *
     * What stage() would do at once for a source's first frame, spread over updates: the
     * interop device opened for OpenGL, the shared planes' D3D11 textures made on a
     * worker thread, then each plane registered with OpenGL, at most one of those GL
     * calls per beginFrame(). Main thread, GL context current.
     */
    SetUp setUp(const YuvPicture& picture, DxInteropTarget& target, std::string& why);

    /**
     * @brief Keep a removed source's target for a later source (TD-35)
     *
     * Unregistering shared planes from OpenGL is one driver call that took up to 57 ms
     * under load even once the GPU was done with them, and registering them again for the
     * next source up to 7 ms a plane. A target whose planes are registered is kept (at
     * most MAX_SPARES), and setUp() gives it to the next fresh target for frames of its
     * size and format, so a clip removed and opened again (undo, a preview, a replaced
     * clip) calls neither. Returns what is not kept, for the caller to release: a target
     * without registered planes, or the oldest spare once there are too many. Null when
     * kept. Main thread.
     */
    std::unique_ptr<DxInteropTarget> recycle(std::unique_ptr<DxInteropTarget> target);
    static constexpr size_t MAX_SPARES = 4;

    /**
     * @brief Hand back a kept target for the caller to release (TD-35)
     *
     * The oldest one kept for SPARE_LIFETIME without a source taking it, or, with `all`,
     * the oldest one whatever its age; null when there is none. A spare holds two plane
     * textures of its size: about 100 MB at 8K P010, so up to 0.4 GB for MAX_SPARES.
     * Main thread.
     */
    std::unique_ptr<DxInteropTarget> takeSpare(bool all = false);
    static constexpr std::chrono::seconds SPARE_LIFETIME{10};

    // Release every kept target now (leak checks, before the GL context goes): main
    // thread, GL context current; it may stall on the driver.
    void releaseSpares();

    // Wait for the worker making shared planes' D3D11 textures (tests, leak checks): it
    // blocks, so never on the live path.
    void finishBackgroundWork();

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

    bool openDevice(void* source, std::string& why);  // source: the decoders' ID3D11Device

    std::shared_ptr<InteropDevice> device_;
    // prepare()'s device, until the main thread opens it for OpenGL. Once openDevice()
    // ran (haveDevice_), prepare() makes no more.
    std::mutex prepareMutex_;
    std::shared_ptr<InteropDevice> prepared_;
    bool preparing_ = false;
    bool haveDevice_ = false;
    std::vector<Pending> pending_;
    // finish()'s working lists, kept so a frame allocates nothing.
    std::vector<Pending> delivering_;
    std::vector<void*> handles_;  // HANDLEs of the shared planes to lock
    std::vector<size_t> copied_;
    std::vector<std::pair<size_t, std::string>> failed_;
    bool checked_ = false;
    bool available_ = false;
    std::string unavailableReason_;
    // The GPU GL renders on, asked with available() (glAdapterLuid).
    bool glLuidKnown_ = false;
    uint64_t glLuid_ = 0;
    std::string glLuidWhy_;
    int batches_ = 0;
    int stepsLeft_ = 1;  // setUp() GL calls left this frame (beginFrame)
    std::unique_ptr<BackgroundWorker> builder_;  // makes shared planes' D3D11 textures
    struct Spare {
        std::unique_ptr<DxInteropTarget> target;
        std::chrono::steady_clock::time_point keptAt;
    };
    std::vector<Spare> spares_;  // recycle(), oldest first
};

}  // namespace quadmapper
