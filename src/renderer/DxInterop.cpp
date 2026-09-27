#include "DxInterop.hpp"
#include "Texture.hpp"
#include "YuvConverter.hpp"
#include "core/GlObjectStats.hpp"
#include "core/Logger.hpp"

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
#include <iterator>

namespace quadmapper {

namespace {

std::atomic<long> g_liveObjects{0};

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
    HMODULE library = LoadLibraryW(L"d3dcompiler_47.dll");
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
    ID3D11Device* device = nullptr;               // owned
    ID3D11DeviceContext* context = nullptr;       // owned
    ID3D11ComputeShader* planeCopy = nullptr;     // owned
    HANDLE handle = nullptr;                      // the interop device

    // Not counted in DxInterop::liveObjects(): one per DxInterop, not per source.
    ~InteropDevice() {
        if (handle) closeDevice(handle);
        if (planeCopy) planeCopy->Release();
        if (context) context->Release();
        if (device) device->Release();
        if (library) FreeLibrary(library);
    }
#endif
};

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

    Input* inputFor(const GpuSurface& surface) {
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

    // Make the shared planes and their registrations for `picture`, whose decoded
    // surface has `format`. On failure the caller releases what exists.
    bool build(const std::shared_ptr<InteropDevice>& owner, const YuvPicture& picture, DXGI_FORMAT format,
               std::string& why) {
        releaseObjects();
        device = owner;
        const DxInterop::TestHooks& hooks = DxInterop::testHooks();
        const std::string test = " (test)";

        if (hooks.rejectFormat || !planeFormats(format, planeFormat)) {
            why = "this pixel format cannot be shared" + (hooks.rejectFormat ? test : "");
            return false;
        }
        for (int p = 0; p < 2; ++p) {
            Plane& plane = planes[p];
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(p == 0 ? picture.width : picture.chromaWidth());
            desc.Height = static_cast<UINT>(p == 0 ? picture.height : picture.chromaHeight());
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
            if (FAILED(device->device->CreateTexture2D(&desc, nullptr, &plane.texture))) {
                plane.texture = nullptr;
                why = "out of video memory for the shared planes";
                return false;
            }
            onCreated();
            if (FAILED(device->device->CreateUnorderedAccessView(plane.texture, nullptr, &plane.output))) {
                plane.output = nullptr;
                why = "the shared planes cannot be written by the GPU";
                return false;
            }
            onCreated();

            glGenTextures(1, &plane.glTexture);
            GlObjectStats::onTextureCreated();
            if (!hooks.failRegistration) {
                plane.registration = device->registerObject(device->handle, plane.texture, plane.glTexture,
                                                            GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
            }
            if (!plane.registration) {
                why = "OpenGL could not register the shared planes" + (hooks.failRegistration ? test : "");
                return false;
            }
            onCreated();
        }
        width = picture.width;
        height = picture.height;
        inputFormat = format;
        return true;
    }
#endif

    void releaseObjects() {
#ifdef _WIN32
        for (Plane& plane : planes) {
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
        for (Input& input : inputs) input.release();
        inputs.clear();
        width = height = 0;
        inputFormat = DXGI_FORMAT_UNKNOWN;
#endif
    }

    ~Impl() { releaseObjects(); }
};

DxInteropTarget::DxInteropTarget() : impl_(std::make_unique<Impl>()) {}
DxInteropTarget::~DxInteropTarget() = default;

// --- DxInterop ---

DxInterop::TestHooks& DxInterop::testHooks() {
    static TestHooks hooks;
    return hooks;
}

long DxInterop::liveObjects() {
    return g_liveObjects.load(std::memory_order_relaxed);
}

DxInterop::DxInterop() = default;

DxInterop::~DxInterop() {
    pending_.clear();
    device_.reset();  // closed here unless a target still holds it
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

bool DxInterop::openDevice(const GpuSurface& surface, std::string& why) {
#ifdef _WIN32
    auto device = std::make_shared<InteropDevice>();
    auto load = [](const char* name) { return glfwGetProcAddress(name); };
    device->openDevice = reinterpret_cast<PFNWGLDXOPENDEVICENVPROC>(load("wglDXOpenDeviceNV"));
    device->closeDevice = reinterpret_cast<PFNWGLDXCLOSEDEVICENVPROC>(load("wglDXCloseDeviceNV"));
    device->registerObject = reinterpret_cast<PFNWGLDXREGISTEROBJECTNVPROC>(load("wglDXRegisterObjectNV"));
    device->unregisterObject = reinterpret_cast<PFNWGLDXUNREGISTEROBJECTNVPROC>(load("wglDXUnregisterObjectNV"));
    device->lockObjects = reinterpret_cast<PFNWGLDXLOCKOBJECTSNVPROC>(load("wglDXLockObjectsNV"));
    device->unlockObjects = reinterpret_cast<PFNWGLDXUNLOCKOBJECTSNVPROC>(load("wglDXUnlockObjectsNV"));
    device->source = surface.device;

    // The decoders' adapter: the shared frames live in its memory.
    IDXGIAdapter* adapter = nullptr;
    if (auto* source = static_cast<ID3D11Device*>(surface.device)) {
        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(source->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) {
            dxgi->GetAdapter(&adapter);
            dxgi->Release();
        }
    }
    using Create = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                    UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    device->library = LoadLibraryW(L"d3d11.dll");
    auto create = device->library ? reinterpret_cast<Create>(GetProcAddress(device->library, "D3D11CreateDevice"))
                                  : nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    if (!device->openDevice || !device->closeDevice || !device->registerObject ||
        !device->unregisterObject || !device->lockObjects || !device->unlockObjects) {
        why = "the WGL_NV_DX_interop2 entry points are missing";
    } else if (!adapter || !surface.shared) {
        why = "the frame carries no shared D3D11 texture";
    } else if (!create || FAILED(create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device->device,
                                        nullptr, &device->context))) {
        device->device = nullptr;
        device->context = nullptr;
        why = "a D3D11 device (feature level 11) could not be created for sharing";
    } else if ((device->planeCopy = compilePlaneCopy(device->device, why)) != nullptr) {
        device->handle = device->openDevice(device->device);
        if (device->handle) {
            adapter->Release();
            device_ = std::move(device);
            return true;
        }
        why = "OpenGL could not open the D3D11 device for sharing";
    }
    if (adapter) adapter->Release();
    // Every stream would fail the same way: stop offering zero-copy.
    available_ = false;
    unavailableReason_ = why;
    Logger::warning("Zero-copy video unavailable ({}); hardware frames are copied through memory", why);
    return false;
#else
    (void)surface;
    why = "zero-copy video needs Windows";
    return false;
#endif
}

bool DxInterop::stage(const YuvPicture& picture, DxInteropTarget& target, Texture& dest, void* key,
                      std::string& why) {
#ifdef _WIN32
    if (!picture.onGpu()) {
        why = "the frame is not on the GPU";
        return false;
    }
    if (!available()) {
        why = unavailableReason_;
        return false;
    }
    if (!device_ && !openDevice(picture.gpu, why)) return false;
    if (picture.gpu.device != device_->source || !picture.gpu.shared) {
        why = "the frame comes from another D3D11 device";
        return false;
    }

    DxInteropTarget::Impl& t = *target.impl_;
    D3D11_TEXTURE2D_DESC inputDesc{};
    static_cast<ID3D11Texture2D*>(picture.gpu.texture)->GetDesc(&inputDesc);
    const bool rebuild = !t.planes[0].texture || t.width != picture.width || t.height != picture.height ||
                         t.inputFormat != inputDesc.Format || t.device != device_;
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

void DxInterop::finish(YuvConverter& converter, const Failure& onFailure) {
    if (pending_.empty()) return;
#ifdef _WIN32
    InteropDevice& device = *device_;
    ID3D11DeviceContext* context = device.context;
    std::vector<HANDLE> handles;
    std::vector<size_t> copied;
    std::vector<std::pair<size_t, std::string>> failed;
    handles.reserve(pending_.size() * 2);

    // 1. Copy every frame's planes into its shared planes, then lock them all for OpenGL.
    // The frame's keyed mutex orders the copy after the decoder's write of it on the GPU.
    context->CSSetShader(device.planeCopy, nullptr, 0);
    for (size_t i = 0; i < pending_.size(); ++i) {
        const Pending& p = pending_[i];
        DxInteropTarget::Impl& t = *p.target->impl_;
        DxInteropTarget::Impl::Input* input = t.inputFor(p.picture.gpu);
        if (!input) {
            failed.emplace_back(i, "the decoded frame could not be opened on the renderer's D3D11 device");
            continue;
        }
        if (input->mutex->AcquireSync(0, ACQUIRE_TIMEOUT_MS) != S_OK) {
            failed.emplace_back(i, "the decoded frame stayed locked by its decoder");
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
        copied.push_back(i);
        handles.push_back(t.planes[0].registration);
        handles.push_back(t.planes[1].registration);
    }
    // A bound shared texture could not be handed to OpenGL.
    ID3D11UnorderedAccessView* noOutputs[2] = {nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 2, noOutputs, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    context->Flush();
    const TestHooks& hooks = testHooks();
    const bool injected = hooks.failLockAfter >= 0 && batches_ >= hooks.failLockAfter;
    const bool locked = !handles.empty() && !injected &&
                        device.lockObjects(device.handle, static_cast<GLint>(handles.size()), handles.data());

    // 2. Convert each source's shared planes into its texture.
    if (locked) {
        for (size_t i : copied) {
            const Pending& p = pending_[i];
            const DxInteropTarget::Impl& t = *p.target->impl_;
            if (!converter.convertPlanes(p.picture, t.planes[0].glTexture, t.planes[1].glTexture, *p.dest)) {
                failed.emplace_back(i, "the shared planes could not be converted in OpenGL");
            }
        }
        // 3. Hand the shared planes back to D3D11.
        device.unlockObjects(device.handle, static_cast<GLint>(handles.size()), handles.data());
        ++batches_;
    } else if (!handles.empty()) {
        const std::string why = std::string("OpenGL could not lock the shared textures") + (injected ? " (test)" : "");
        for (size_t i : copied) failed.emplace_back(i, why);
    }

    // The failure callback may destroy the failed source's target: pending_ is done with.
    std::vector<Pending> done;
    done.swap(pending_);
    for (const auto& [index, why] : failed) onFailure(done[index].key, done[index].picture, why);
#else
    (void)converter;
    for (const Pending& p : pending_) onFailure(p.key, p.picture, "zero-copy video needs Windows");
    pending_.clear();
#endif
}

}  // namespace quadmapper
