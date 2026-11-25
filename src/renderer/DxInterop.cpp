#include "DxInterop.hpp"
#include "Texture.hpp"
#include "YuvConverter.hpp"
#include "core/BackgroundWorker.hpp"
#include "core/GlObjectStats.hpp"
#include "core/Logger.hpp"
#include "core/MainThreadTimings.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>  // the D3DCompile signature only; the DLL is loaded at run time
#include <dxgi.h>
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>

namespace quadmapper {

namespace {

std::atomic<long> g_liveObjects{0};
std::atomic<long> g_liveDevices{0};

void onCreated() { g_liveObjects.fetch_add(1, std::memory_order_relaxed); }
void onReleased() { g_liveObjects.fetch_sub(1, std::memory_order_relaxed); }

#ifdef _WIN32
// WGL_NV_DX_interop / WGL_NV_DX_interop2 (Khronos wglext.h); GLAD loads GL only.
using PFNWGLDXOPENDEVICENVPROC = HANDLE(WINAPI*)(void* dxDevice);
using PFNWGLDXCLOSEDEVICENVPROC = BOOL(WINAPI*)(HANDLE device);
using PFNWGLDXREGISTEROBJECTNVPROC = HANDLE(WINAPI*)(HANDLE device, void* dxObject, GLuint name,
                                                     GLenum type, GLenum access);
using PFNWGLDXUNREGISTEROBJECTNVPROC = BOOL(WINAPI*)(HANDLE device, HANDLE object);
using PFNWGLDXLOCKOBJECTSNVPROC = BOOL(WINAPI*)(HANDLE device, GLint count, HANDLE* objects);
using PFNWGLDXUNLOCKOBJECTSNVPROC = BOOL(WINAPI*)(HANDLE device, GLint count, HANDLE* objects);
constexpr GLenum WGL_ACCESS_READ_ONLY_NV = 0x0000;

// GL_EXT_memory_object / GL_EXT_semaphore (with their _win32 variants): the LUID of the
// adapter the GL context renders on. GLAD loads GL only.
using PFNGLGETUNSIGNEDBYTEVEXTPROC = void(APIENTRY*)(GLenum pname, GLubyte* data);
constexpr GLenum GL_DEVICE_LUID_EXT = 0x9599;

// System DLLs load from System32 only, never from the working or application directory.
HMODULE loadSystemLibrary(const wchar_t* name) {
    return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

// Decoded frames opened per source: its GPU pool's textures, reused round-robin.
constexpr size_t MAX_INPUTS = 16;
constexpr UINT GROUP_WIDTH = 16;
constexpr UINT GROUP_HEIGHT = 8;
// A decoded frame's keyed mutex is free unless its decoder is copying it for a fallback
// readback; past this the frame is dropped from the zero-copy path.
constexpr DWORD ACQUIRE_TIMEOUT_MS = 100;

// Copies an NV12 or P010 frame's two planes, read through its plane views, into the
// plane textures OpenGL shares. 8- and 16-bit unorm samples survive the float round trip
// exactly. Threads past the smaller chroma plane write nothing (out-of-range UAV writes
// are dropped).
constexpr char PLANE_COPY_HLSL[] = R"HLSL(
Texture2D<float> srcLuma : register(t0);
Texture2D<float2> srcChroma : register(t1);
RWTexture2D<unorm float> dstLuma : register(u0);
RWTexture2D<unorm float2> dstChroma : register(u1);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    dstLuma[id.xy] = srcLuma[id.xy];
    dstChroma[id.xy] = srcChroma[id.xy];
}
)HLSL";

template <typename T>
void release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
        onReleased();
    }
}

// The single-plane formats a surface's luma and chroma planes are viewed and shared as.
bool planeFormats(DXGI_FORMAT surface, DXGI_FORMAT (&planes)[2]) {
    switch (surface) {
        case DXGI_FORMAT_NV12:
            planes[0] = DXGI_FORMAT_R8_UNORM;
            planes[1] = DXGI_FORMAT_R8G8_UNORM;
            return true;
        case DXGI_FORMAT_P010:
            planes[0] = DXGI_FORMAT_R16_UNORM;
            planes[1] = DXGI_FORMAT_R16G16_UNORM;
            return true;
        default:
            return false;
    }
}

// The plane-copy shader, compiled with the system's d3dcompiler_47.dll (Windows 10+).
ID3D11ComputeShader* compilePlaneCopy(ID3D11Device* device, std::string& why) {
    HMODULE library = loadSystemLibrary(L"d3dcompiler_47.dll");
    if (!library) {
        why = "d3dcompiler_47.dll is missing";
        return nullptr;
    }
    auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(library, "D3DCompile"));
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    ID3D11ComputeShader* shader = nullptr;
    if (!compile || FAILED(compile(PLANE_COPY_HLSL, sizeof(PLANE_COPY_HLSL) - 1, "PlaneCopy", nullptr,
                                   nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                                   &errors))) {
        why = "the plane copy shader did not compile";
        if (errors) Logger::warning("DxInterop: {}", static_cast<const char*>(errors->GetBufferPointer()));
    } else if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                  &shader))) {
        shader = nullptr;
        why = "the D3D11 device refused the plane copy shader";
    }
    if (code) code->Release();
    if (errors) errors->Release();
    FreeLibrary(library);
    return shader;
}
#endif

}  // namespace

// The renderer's own D3D11 device, on the decoders' adapter, opened for OpenGL sharing.
// Only the main thread uses it, so no call here waits on the decoders: they keep their
// device's immediate context, which 16 streams keep busy. Sharing that context stalled
// the main thread up to 290 ms at a loop seam (ADR-056 amendment).
struct InteropDevice {
#ifdef _WIN32
    PFNWGLDXOPENDEVICENVPROC openDevice = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC closeDevice = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC registerObject = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC unregisterObject = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC lockObjects = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC unlockObjects = nullptr;

    HMODULE library = nullptr;                    // d3d11.dll, held while the device lives
    void* source = nullptr;                       // the decoders' device frames come from
    bool haveLuid = false;
    uint64_t luid = 0;                            // the decoders' adapter, when known
    ID3D11Device* device = nullptr;               // owned
    ID3D11DeviceContext* context = nullptr;       // owned
    ID3D11ComputeShader* planeCopy = nullptr;     // owned
    HANDLE handle = nullptr;                      // the interop device

    // Counted in DxInterop::liveDevices(), not liveObjects(): one per DxInterop, not per source.
    InteropDevice() { g_liveDevices.fetch_add(1, std::memory_order_relaxed); }
    ~InteropDevice() {
        g_liveDevices.fetch_sub(1, std::memory_order_relaxed);
        if (handle) closeDevice(handle);
        if (planeCopy) planeCopy->Release();
        if (context) context->Release();
        if (device) device->Release();
        if (library) FreeLibrary(library);
    }
#endif
};

#ifdef _WIN32
// The D3D11 half of a source's two shared planes: textures the plane copy writes, made on
// any thread (the D3D11 device is free-threaded). make() fills it; a worker publishes it
// through `done`. What it holds and was not taken is released with it.
struct MadePlanes {
    std::shared_ptr<InteropDevice> device;
    int width = 0;
    int height = 0;
    int chromaWidth = 0;
    int chromaHeight = 0;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT planeFormat[2] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    ID3D11Texture2D* texture[2] = {nullptr, nullptr};
    ID3D11UnorderedAccessView* output[2] = {nullptr, nullptr};
    bool ok = false;
    std::string why;
    std::atomic<bool> done{false};

    MadePlanes(std::shared_ptr<InteropDevice> owner, const YuvPicture& picture, DXGI_FORMAT format)
        : device(std::move(owner)), width(picture.width), height(picture.height),
          chromaWidth(picture.chromaWidth()), chromaHeight(picture.chromaHeight()), inputFormat(format) {}
    ~MadePlanes() {
        for (int p = 0; p < 2; ++p) {
            release(output[p]);
            release(texture[p]);
        }
    }
    MadePlanes(const MadePlanes&) = delete;
    MadePlanes& operator=(const MadePlanes&) = delete;

    bool fits(const YuvPicture& picture, DXGI_FORMAT format) const {
        return width == picture.width && height == picture.height && inputFormat == format;
    }

    void make() {
        ok = makePlanes();
        done.store(true, std::memory_order_release);
    }

private:
    bool makePlanes() {
        const bool reject = DxInterop::testHooks().rejectFormat;
        if (reject || !planeFormats(inputFormat, planeFormat)) {
            why = std::string("this pixel format cannot be shared") + (reject ? " (test)" : "");
            return false;
        }
        for (int p = 0; p < 2; ++p) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(p == 0 ? width : chromaWidth);
            desc.Height = static_cast<UINT>(p == 0 ? height : chromaHeight);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = planeFormat[p];
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            // Without SHARED, NVIDIA's interop gives OpenGL a copy taken at registration:
            // every later write stays invisible and the source shows black (measured, RTX
            // 3080 Laptop, ADR-056 amendment).
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            if (FAILED(device->device->CreateTexture2D(&desc, nullptr, &texture[p]))) {
                texture[p] = nullptr;
                why = "out of video memory for the shared planes";
                return false;
            }
            onCreated();
            if (FAILED(device->device->CreateUnorderedAccessView(texture[p], nullptr, &output[p]))) {
                output[p] = nullptr;
                why = "the shared planes cannot be written by the GPU";
                return false;
            }
            onCreated();
        }
        return true;
    }
};
#endif

// --- DxInteropTarget ---

struct DxInteropTarget::Impl {
    std::shared_ptr<InteropDevice> device;
#ifdef _WIN32
    // What the objects below were made for.
    int width = 0;
    int height = 0;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT planeFormat[2] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};

    // A shared plane: the D3D11 texture the copy writes, and its GL name.
    struct Plane {
        ID3D11Texture2D* texture = nullptr;
        ID3D11UnorderedAccessView* output = nullptr;
        GLuint glTexture = 0;
        HANDLE registration = nullptr;
    };
    Plane planes[2];

    // One decoded frame's texture, opened on this device, with its plane views.
    struct Input {
        uint64_t serial = 0;
        ID3D11Texture2D* texture = nullptr;
        IDXGIKeyedMutex* mutex = nullptr;
        ID3D11ShaderResourceView* views[2] = {nullptr, nullptr};

        void release() {
            for (ID3D11ShaderResourceView*& view : views) quadmapper::release(view);
            quadmapper::release(mutex);
            quadmapper::release(texture);
        }
    };
    std::vector<Input> inputs;  // least recently used first
    uint64_t pool = 0;          // the decoder pool `inputs` come from

    Input* inputFor(const GpuSurface& surface) {
        if (surface.pool != pool) {
            // The decoder made a new pool: the old one's textures are never seen again,
            // and holding them open would keep their video memory.
            releaseInputs();
            pool = surface.pool;
        }
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (inputs[i].serial == surface.serial) {
                std::rotate(inputs.begin() + static_cast<std::ptrdiff_t>(i),
                            inputs.begin() + static_cast<std::ptrdiff_t>(i) + 1, inputs.end());
                return &inputs.back();
            }
        }
        Input made;
        made.serial = surface.serial;
        bool ok = SUCCEEDED(device->device->OpenSharedResource(static_cast<HANDLE>(surface.shared),
                                                                __uuidof(ID3D11Texture2D),
                                                                reinterpret_cast<void**>(&made.texture)));
        if (ok) {
            onCreated();
            ok = SUCCEEDED(made.texture->QueryInterface(__uuidof(IDXGIKeyedMutex),
                                                        reinterpret_cast<void**>(&made.mutex)));
            if (ok) onCreated();
        }
        for (int p = 0; ok && p < 2; ++p) {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = planeFormat[p];
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = 1;
            ok = SUCCEEDED(device->device->CreateShaderResourceView(made.texture, &desc, &made.views[p]));
            if (ok) onCreated();
            else made.views[p] = nullptr;
        }
        if (!ok) {
            made.release();
            return nullptr;
        }
        if (inputs.size() >= MAX_INPUTS) {
            inputs.front().release();
            inputs.erase(inputs.begin());
        }
        inputs.push_back(made);
        return &inputs.back();
    }

    // Planes being made on DxInterop's worker for setUp() (null otherwise).
    std::shared_ptr<MadePlanes> making;

    // Whether the shared planes exist for frames of `format` like `picture`, from `owner`.
    bool fits(const std::shared_ptr<InteropDevice>& owner, const YuvPicture& picture, DXGI_FORMAT format) const {
        return planes[0].texture && device == owner && width == picture.width && height == picture.height &&
               inputFormat == format;
    }
    bool registered() const { return planes[0].registration && planes[1].registration; }

    // Take the D3D11 planes `made` made, in place of any held.
    void adopt(MadePlanes& made) {
        releaseObjects();
        device = made.device;
        for (int p = 0; p < 2; ++p) {
            planeFormat[p] = made.planeFormat[p];
            planes[p].texture = std::exchange(made.texture[p], nullptr);
            planes[p].output = std::exchange(made.output[p], nullptr);
        }
        width = made.width;
        height = made.height;
        inputFormat = made.inputFormat;
    }

    // Name plane `p` in GL and register it with the interop device: GL thread.
    bool registerPlane(int p, std::string& why) {
        ScopedMainThreadTiming timing("zeroCopyPlaneRegister");
        Plane& plane = planes[p];
        const bool fail = DxInterop::testHooks().failRegistration;
        if (plane.glTexture == 0) {
            glGenTextures(1, &plane.glTexture);
            GlObjectStats::onTextureCreated();
        }
        if (!fail) {
            plane.registration = device->registerObject(device->handle, plane.texture, plane.glTexture,
                                                        GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        }
        if (!plane.registration) {
            why = std::string("OpenGL could not register the shared planes") + (fail ? " (test)" : "");
            return false;
        }
        onCreated();
        return true;
    }

    // Make the shared planes and their registrations for `picture`, whose decoded
    // surface has `format`, here and now. On failure the caller releases what exists.
    bool build(const std::shared_ptr<InteropDevice>& owner, const YuvPicture& picture, DXGI_FORMAT format,
               std::string& why) {
        MadePlanes made(owner, picture, format);
        made.make();
        if (!made.ok) {
            why = made.why;
            return false;
        }
        adopt(made);
        return registerPlane(0, why) && registerPlane(1, why);
    }
#endif

#ifdef _WIN32
    void releaseInputs() {
        for (Input& input : inputs) input.release();
        inputs.clear();
    }
#endif

#ifdef _WIN32
    static bool held(const Plane& plane) {
        return plane.registration || plane.glTexture != 0 || plane.output || plane.texture;
    }

    // Unregistered from GL before either side's texture is released (ADR-056).
    void releasePlane(Plane& plane) {
        if (plane.registration) {
            device->unregisterObject(device->handle, plane.registration);
            plane.registration = nullptr;
            onReleased();
        }
        if (plane.glTexture != 0) {
            glDeleteTextures(1, &plane.glTexture);
            GlObjectStats::onTextureDestroyed();
            plane.glTexture = 0;
        }
        release(plane.output);
        release(plane.texture);
    }

    void releaseRest() {
        making.reset();  // a worker still making planes frees them when done
        releaseInputs();
        width = height = 0;
        inputFormat = DXGI_FORMAT_UNKNOWN;
    }
#endif

    bool releaseStep() {
#ifdef _WIN32
        for (Plane& plane : planes) {
            if (held(plane)) {
                releasePlane(plane);
                break;
            }
        }
        for (const Plane& plane : planes) {
            if (held(plane)) return false;
        }
        releaseRest();
#endif
        return true;
    }

    void releaseObjects() {
#ifdef _WIN32
        for (Plane& plane : planes) releasePlane(plane);
        releaseRest();
#endif
    }

    ~Impl() { releaseObjects(); }
};

DxInteropTarget::DxInteropTarget() : impl_(std::make_unique<Impl>()) {}
DxInteropTarget::~DxInteropTarget() = default;

bool DxInteropTarget::releaseStep() {
    return impl_->releaseStep();
}

// --- DxInterop ---

DxInterop::TestHooks& DxInterop::testHooks() {
    static TestHooks hooks;
    return hooks;
}

long DxInterop::liveObjects() {
    return g_liveObjects.load(std::memory_order_relaxed);
}

long DxInterop::liveDevices() {
    return g_liveDevices.load(std::memory_order_relaxed);
}

DxInterop::DxInterop() = default;

DxInterop::~DxInterop() {
    pending_.clear();
    builder_.reset();  // joined first: its tasks hold the device, which must close here
    spares_.clear();
    device_.reset();   // closed here unless a target still holds it
}

std::unique_ptr<DxInteropTarget> DxInterop::recycle(std::unique_ptr<DxInteropTarget> target) {
    if (!target) return nullptr;
#ifdef _WIN32
    DxInteropTarget::Impl& t = *target->impl_;
    if (!device_ || t.device != device_ || !t.registered()) return target;
    t.releaseInputs();  // the decoded frames of its old source: never seen again
#else
    return target;
#endif
    spares_.push_back({std::move(target), std::chrono::steady_clock::now()});
    if (spares_.size() <= MAX_SPARES) return nullptr;
    return takeSpare(true);
}

std::unique_ptr<DxInteropTarget> DxInterop::takeSpare(bool all) {
    if (spares_.empty()) return nullptr;
    if (!all) {
        const int hookMs = testHooks().spareLifetimeMs;
        const std::chrono::steady_clock::duration lifetime =
            hookMs >= 0 ? std::chrono::steady_clock::duration(std::chrono::milliseconds(hookMs))
                        : std::chrono::steady_clock::duration(SPARE_LIFETIME);
        if (std::chrono::steady_clock::now() - spares_.front().keptAt < lifetime) return nullptr;
    }
    std::unique_ptr<DxInteropTarget> oldest = std::move(spares_.front().target);
    spares_.erase(spares_.begin());
    return oldest;
}

void DxInterop::releaseSpares() {
    spares_.clear();
}

void DxInterop::finishBackgroundWork() {
    if (builder_) builder_->drain();
}

bool DxInterop::available() {
    if (checked_) return available_;
    checked_ = true;
#ifdef _WIN32
    if (testHooks().extensionMissing) {
        unavailableReason_ = "the OpenGL driver lacks WGL_NV_DX_interop2 (test)";
    } else if (!glfwGetCurrentContext()) {
        checked_ = false;  // ask again once there is a context
        unavailableReason_ = "no OpenGL context";
    } else if (!glfwExtensionSupported("WGL_NV_DX_interop2")) {
        unavailableReason_ = "the OpenGL driver lacks WGL_NV_DX_interop2";
    } else {
        available_ = true;
        // Which GPU GL renders on, asked now (SourceManager::prepareGpu at start-up) and
        // not when the interop device opens: the glGet calls wait for the driver to catch
        // up with the commands queued, up to 25 ms in a busy show (TD-35).
        ScopedMainThreadTiming timing("glAdapterLuidQuery");
        glLuidKnown_ = glAdapterLuid(glLuid_, glLuidWhy_);
    }
#else
    unavailableReason_ = "zero-copy video needs Windows";
#endif
    if (!available_ && checked_) {
        Logger::info("Zero-copy video unavailable ({}); hardware frames are copied through memory",
                     unavailableReason_);
    }
    return available_;
}

bool DxInterop::glAdapterLuid(uint64_t& luid, std::string& why) {
#ifdef _WIN32
    if (!glfwExtensionSupported("GL_EXT_memory_object_win32") && !glfwExtensionSupported("GL_EXT_semaphore_win32")) {
        why = "neither GL_EXT_memory_object_win32 nor GL_EXT_semaphore_win32";
        return false;
    }
    auto get = reinterpret_cast<PFNGLGETUNSIGNEDBYTEVEXTPROC>(glfwGetProcAddress("glGetUnsignedBytevEXT"));
    if (!get) {
        why = "no glGetUnsignedBytevEXT";
        return false;
    }
    // An error left by earlier GL work would read as this query failing: in the app the
    // first zero-copy frame always met one, so the adapter check never ran.
    while (glGetError() != GL_NO_ERROR) {}
    GLubyte bytes[sizeof(luid)] = {};
    get(GL_DEVICE_LUID_EXT, bytes);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        why = "GL_DEVICE_LUID_EXT raised GL error " + std::to_string(error);
        return false;
    }
    std::memcpy(&luid, bytes, sizeof(luid));
    return true;
#else
    (void)luid;
    why = "not Windows";
    return false;
#endif
}

#ifdef _WIN32
namespace {

// The D3D11 half of opening the interop device, no GL: a device on the adapter of the
// decoders' device `source`, and the plane copy shader. Null, with `why`, on failure.
std::shared_ptr<InteropDevice> createInteropDevice(void* source, std::string& why) {
    auto device = std::make_shared<InteropDevice>();
    device->source = source;

    // The decoders' adapter: the shared frames live in its memory.
    IDXGIAdapter* adapter = nullptr;
    if (auto* decoders = static_cast<ID3D11Device*>(source)) {
        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(decoders->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) {
            dxgi->GetAdapter(&adapter);
            dxgi->Release();
        }
    }
    if (!adapter) {
        why = "the frame carries no shared D3D11 texture";
        return nullptr;
    }
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
        std::memcpy(&device->luid, &adapterDesc.AdapterLuid, sizeof(device->luid));
        device->haveLuid = true;
    }
    using Create = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                    UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    device->library = loadSystemLibrary(L"d3d11.dll");
    auto create = device->library ? reinterpret_cast<Create>(GetProcAddress(device->library, "D3D11CreateDevice"))
                                  : nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const bool created = create && SUCCEEDED(create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                                    static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                                    &device->device, nullptr, &device->context));
    adapter->Release();
    if (!created) {
        device->device = nullptr;
        device->context = nullptr;
        why = "a D3D11 device (feature level 11) could not be created for sharing";
        return nullptr;
    }
    device->planeCopy = compilePlaneCopy(device->device, why);
    if (!device->planeCopy) return nullptr;
    return device;
}

}  // namespace
#endif

void DxInterop::prepare(void* decoderDevice) {
#ifdef _WIN32
    if (!decoderDevice) return;
    {
        std::lock_guard<std::mutex> lock(prepareMutex_);
        if (haveDevice_ || prepared_ || preparing_) return;
        preparing_ = true;
    }
    // Not under the lock: the main thread never waits for this. If it needs a device
    // first, it makes its own and this one is dropped below.
    std::string why;
    std::shared_ptr<InteropDevice> device = createInteropDevice(decoderDevice, why);
    if (!device) Logger::info("Zero-copy video: preparing the interop device failed ({}); trying at the first frame", why);
    std::lock_guard<std::mutex> lock(prepareMutex_);
    preparing_ = false;
    if (!haveDevice_) prepared_ = std::move(device);
#else
    (void)decoderDevice;
#endif
}

bool DxInterop::openDevice(void* source, std::string& why) {
#ifdef _WIN32
    ScopedMainThreadTiming timing("zeroCopyDeviceOpen");
    std::shared_ptr<InteropDevice> device;
    {
        // From here on prepare() makes none; one it made for another decode device is
        // of no use.
        std::lock_guard<std::mutex> lock(prepareMutex_);
        haveDevice_ = true;
        if (prepared_ && prepared_->source == source) device = std::move(prepared_);
        prepared_.reset();
    }
    auto load = [](const char* name) { return glfwGetProcAddress(name); };
    const auto openDevice = reinterpret_cast<PFNWGLDXOPENDEVICENVPROC>(load("wglDXOpenDeviceNV"));
    const auto closeDevice = reinterpret_cast<PFNWGLDXCLOSEDEVICENVPROC>(load("wglDXCloseDeviceNV"));
    const auto registerObject = reinterpret_cast<PFNWGLDXREGISTEROBJECTNVPROC>(load("wglDXRegisterObjectNV"));
    const auto unregisterObject = reinterpret_cast<PFNWGLDXUNREGISTEROBJECTNVPROC>(load("wglDXUnregisterObjectNV"));
    const auto lockObjects = reinterpret_cast<PFNWGLDXLOCKOBJECTSNVPROC>(load("wglDXLockObjectsNV"));
    const auto unlockObjects = reinterpret_cast<PFNWGLDXUNLOCKOBJECTSNVPROC>(load("wglDXUnlockObjectsNV"));

    if (!openDevice || !closeDevice || !registerObject || !unregisterObject || !lockObjects || !unlockObjects) {
        why = "the WGL_NV_DX_interop2 entry points are missing";
    } else if (device || (device = createInteropDevice(source, why))) {
        // On a hybrid laptop GL may render on one GPU while the decoders use the other. The
        // extension does not promise sharing between two GPUs (a driver may refuse it or
        // copy through system memory), so compare the adapters where GL names its own.
        bool otherAdapter = testHooks().adapterMismatch;
        if (!otherAdapter && device->haveLuid) {
            if (glLuidKnown_) {
                otherAdapter = glLuid_ != device->luid;
                Logger::info("Zero-copy video: OpenGL renders on GPU {} and the video decoders on GPU {}{}", glLuid_,
                             device->luid, otherAdapter ? " (different GPUs)" : " (the same GPU)");
            } else {
                Logger::info("Zero-copy video: the OpenGL driver does not say which GPU it renders on ({}), "
                             "so it is assumed to be the video decoders' GPU", glLuidWhy_);
            }
        }
        if (otherAdapter) {
            why = std::string("OpenGL renders on another GPU than the video decoders") +
                  (testHooks().adapterMismatch ? " (test)" : "");
        } else {
            device->openDevice = openDevice;
            device->closeDevice = closeDevice;
            device->registerObject = registerObject;
            device->unregisterObject = unregisterObject;
            device->lockObjects = lockObjects;
            device->unlockObjects = unlockObjects;
            device->handle = openDevice(device->device);
            if (device->handle) {
                device_ = std::move(device);
                return true;
            }
            why = "OpenGL could not open the D3D11 device for sharing";
        }
    }
    // Every stream would fail the same way: stop offering zero-copy.
    available_ = false;
    unavailableReason_ = why;
    Logger::warning("Zero-copy video unavailable ({}); hardware frames are copied through memory", why);
    return false;
#else
    (void)source;
    why = "zero-copy video needs Windows";
    return false;
#endif
}

bool DxInterop::stage(const YuvPicture& picture, DxInteropTarget& target, Texture& dest, void* key,
                      std::string& why) {
#ifdef _WIN32
    ScopedMainThreadTiming timing("zeroCopyStage");
    if (!picture.onGpu()) {
        why = "the frame is not on the GPU";
        return false;
    }
    if (!available()) {
        why = unavailableReason_;
        return false;
    }
    if (!picture.gpu.shared) {
        why = "the frame carries no shared D3D11 texture";
        return false;
    }
    if (!device_ && !openDevice(picture.gpu.device, why)) return false;
    if (picture.gpu.device != device_->source || !picture.gpu.shared) {
        why = "the frame comes from another D3D11 device";
        return false;
    }

    DxInteropTarget::Impl& t = *target.impl_;
    D3D11_TEXTURE2D_DESC inputDesc{};
    static_cast<ID3D11Texture2D*>(picture.gpu.texture)->GetDesc(&inputDesc);
    const bool rebuild = !t.fits(device_, picture, inputDesc.Format) || !t.registered();
    ScopedMainThreadTiming buildTiming(rebuild ? "zeroCopyTargetBuild" : "zeroCopyTargetReuse");
    if (rebuild && !t.build(device_, picture, inputDesc.Format, why)) {
        t.releaseObjects();
        return false;
    }
    pending_.push_back({picture, &target, &dest, key});
    return true;
#else
    (void)picture; (void)target; (void)dest; (void)key;
    why = "zero-copy video needs Windows";
    return false;
#endif
}

void DxInterop::beginFrame() {
    stepsLeft_ = 1;
#ifdef _WIN32
    // The opener prepared the device for the first stream on the GPU: open it for OpenGL
    // now, in a frame of its own, before that stream's first frame needs it.
    if (device_ || !checked_ || !available_) return;
    void* source = nullptr;
    {
        std::lock_guard<std::mutex> lock(prepareMutex_);
        if (prepared_) source = prepared_->source;
    }
    if (!source) return;
    --stepsLeft_;
    std::string why;
    openDevice(source, why);  // a failure is logged and ends zero-copy for the app
#endif
}

DxInterop::SetUp DxInterop::setUp(const YuvPicture& picture, DxInteropTarget& target, std::string& why) {
#ifdef _WIN32
    if (!picture.onGpu() || !picture.gpu.shared) {
        why = picture.onGpu() ? "the frame carries no shared D3D11 texture" : "the frame is not on the GPU";
        return SetUp::Failed;
    }
    if (!available()) {
        why = unavailableReason_;
        return SetUp::Failed;
    }
    if (!device_) {
        if (stepsLeft_ == 0) return SetUp::Later;
        --stepsLeft_;
        return openDevice(picture.gpu.device, why) ? SetUp::Later : SetUp::Failed;
    }
    if (picture.gpu.device != device_->source) {
        why = "the frame comes from another D3D11 device";
        return SetUp::Failed;
    }

    DxInteropTarget::Impl& t = *target.impl_;
    D3D11_TEXTURE2D_DESC inputDesc{};
    static_cast<ID3D11Texture2D*>(picture.gpu.texture)->GetDesc(&inputDesc);
    if (!t.fits(device_, picture, inputDesc.Format) && !t.planes[0].texture && !t.making) {
        // A fresh target: a kept one with planes for these frames, already registered.
        for (auto it = spares_.begin(); it != spares_.end(); ++it) {
            if (it->target->impl_->fits(device_, picture, inputDesc.Format)) {
                std::swap(target.impl_, it->target->impl_);
                spares_.erase(it);  // with the fresh, empty one it now holds
                return SetUp::Ready;
            }
        }
    }
    if (!t.fits(device_, picture, inputDesc.Format)) {
        // The D3D11 planes are made on the worker (up to 39 ms each under load, TD-35).
        if (t.making && !t.making->done.load(std::memory_order_acquire)) return SetUp::Later;
        if (t.making && t.making->fits(picture, inputDesc.Format) && t.making->device == device_) {
            std::shared_ptr<MadePlanes> made = std::move(t.making);
            if (!made->ok) {
                why = made->why;
                t.releaseObjects();
                return SetUp::Failed;
            }
            t.adopt(*made);
        } else {
            t.releaseObjects();  // planes for another size or format: the stream changed
            t.making = std::make_shared<MadePlanes>(device_, picture, inputDesc.Format);
            if (!builder_) builder_ = std::make_unique<BackgroundWorker>("Zero-copy planes");
            builder_->post([made = t.making] { made->make(); });
            return SetUp::Later;
        }
    }
    // One registration a frame (up to 13 ms each under load).
    for (int p = 0; p < 2; ++p) {
        if (t.planes[p].registration) continue;
        if (stepsLeft_ == 0) return SetUp::Later;
        --stepsLeft_;
        if (!t.registerPlane(p, why)) {
            t.releaseObjects();
            return SetUp::Failed;
        }
    }
    return SetUp::Ready;
#else
    (void)picture; (void)target;
    why = "zero-copy video needs Windows";
    return SetUp::Failed;
#endif
}

void DxInterop::finish(YuvConverter& converter, const Failure& onFailure) {
    if (pending_.empty()) return;
    ScopedMainThreadTiming timing("zeroCopyFinish");
    // The failure callback may destroy a failed source's target, and stage() must find
    // pending_ empty afterwards: the batch is delivered from delivering_.
    delivering_.swap(pending_);
#ifdef _WIN32
    InteropDevice& device = *device_;
    ID3D11DeviceContext* context = device.context;
    handles_.clear();
    copied_.clear();
    failed_.clear();

    // 1. Copy every frame's planes into its shared planes, then lock them all for OpenGL.
    // The frame's keyed mutex orders the copy after the decoder's write of it on the GPU.
    context->CSSetShader(device.planeCopy, nullptr, 0);
    for (size_t i = 0; i < delivering_.size(); ++i) {
        const Pending& p = delivering_[i];
        DxInteropTarget::Impl& t = *p.target->impl_;
        DxInteropTarget::Impl::Input* input = t.inputFor(p.picture.gpu);
        if (!input) {
            failed_.emplace_back(i, "the decoded frame could not be opened on the renderer's D3D11 device");
            continue;
        }
        if (input->mutex->AcquireSync(0, ACQUIRE_TIMEOUT_MS) != S_OK) {
            failed_.emplace_back(i, "the decoded frame stayed locked by its decoder");
            continue;
        }
        ID3D11UnorderedAccessView* outputs[2] = {t.planes[0].output, t.planes[1].output};
        context->CSSetShaderResources(0, 2, input->views);
        context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
        context->Dispatch((static_cast<UINT>(p.picture.width) + GROUP_WIDTH - 1) / GROUP_WIDTH,
                          (static_cast<UINT>(p.picture.height) + GROUP_HEIGHT - 1) / GROUP_HEIGHT, 1);
        // Unbound before the release: the decoder may write the frame's texture next.
        ID3D11ShaderResourceView* noInputs[2] = {nullptr, nullptr};
        context->CSSetShaderResources(0, 2, noInputs);
        input->mutex->ReleaseSync(0);
        copied_.push_back(i);
        handles_.push_back(t.planes[0].registration);
        handles_.push_back(t.planes[1].registration);
    }
    // A bound shared texture could not be handed to OpenGL.
    ID3D11UnorderedAccessView* noOutputs[2] = {nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 2, noOutputs, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    context->Flush();

    const TestHooks& hooks = testHooks();
    const bool injected = hooks.failLockAfter >= 0 && batches_ >= hooks.failLockAfter;
    // Lock `count` frames' planes (their handles from `first`), or fail as a test forces.
    auto lock = [&](size_t first, size_t count) {
        if (injected) return false;
        if (hooks.failLockDest) {
            for (size_t k = 0; k < count; ++k) {
                if (delivering_[copied_[first + k]].dest == hooks.failLockDest) return false;
            }
        }
        return device.lockObjects(device.handle, static_cast<GLint>(count * 2), &handles_[first * 2]) != FALSE;
    };
    auto unlock = [&](size_t first, size_t count) {
        device.unlockObjects(device.handle, static_cast<GLint>(count * 2), &handles_[first * 2]);
    };
    // 2. Convert each source's shared planes into its texture.
    auto convert = [&](size_t k) {
        const Pending& p = delivering_[copied_[k]];
        const DxInteropTarget::Impl& t = *p.target->impl_;
        if (!converter.convertPlanes(p.picture, t.planes[0].glTexture, t.planes[1].glTexture, *p.dest)) {
            failed_.emplace_back(copied_[k], "the shared planes could not be converted in OpenGL");
        }
    };
    bool anyLocked = false;
    if (!copied_.empty() && lock(0, copied_.size())) {
        for (size_t k = 0; k < copied_.size(); ++k) convert(k);
        unlock(0, copied_.size());  // 3. Hand the shared planes back to D3D11.
        anyLocked = true;
    } else if (!copied_.empty()) {
        // One lock covers every source, so one bad source fails it for all: lock each
        // source alone, and only those whose own lock fails fall back.
        const std::string why = std::string("OpenGL could not lock the shared textures") + (injected ? " (test)" : "");
        for (size_t k = 0; k < copied_.size(); ++k) {
            if (lock(k, 1)) {
                convert(k);
                unlock(k, 1);
                anyLocked = true;
            } else {
                failed_.emplace_back(copied_[k], why);
            }
        }
    }
    if (anyLocked) ++batches_;

    for (const auto& [index, why] : failed_) onFailure(delivering_[index].key, delivering_[index].picture, why);
    failed_.clear();
#else
    (void)converter;
    for (const Pending& p : delivering_) onFailure(p.key, p.picture, "zero-copy video needs Windows");
#endif
    delivering_.clear();
}

}  // namespace quadmapper
