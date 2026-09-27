// Zero-copy video (ADR-056): NV12 and P010 colour bars in D3D11 textures, built with each
// matrix (BT.601, 709, 2020) and range (limited, full), have their planes copied into
// textures shared through WGL_NV_DX_interop2 and converted by YuvConverter into an RGBA
// GL texture that matches the reference RGB within +-2 per channel, the gate the
// readback path's YuvConverterTest holds. Each forced fallback (extension missing,
// registration refused, format refused, lock failure) is reported, and every D3D11 and
// GL object made is released again.
//
// Needs a GL 3.3 context and a D3D11 device on the same GPU that the driver can share;
// SKIPs otherwise (headless CI, drivers without the extension).

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>  // ID3D10Multithread, as FFmpeg's D3D11VA device sets it
#include <dxgi.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "../video/ColourBars.hpp"
#include "core/GlObjectStats.hpp"
#include "renderer/DxInterop.hpp"
#include "renderer/Texture.hpp"
#include "renderer/YuvConverter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace quadmapper;
using namespace quadmapper::colourbars;

namespace {

struct GLContext {
    GLFWwindow* win = nullptr;
    bool ok = false;
    GLContext() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        win = glfwCreateWindow(64, 64, "interop-test", nullptr, nullptr);
        if (!win) { glfwTerminate(); return; }
        glfwMakeContextCurrent(win);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(win); win = nullptr; glfwTerminate(); return;
        }
        ok = true;
    }
    ~GLContext() {
        if (win) { glfwDestroyWindow(win); glfwTerminate(); }
    }
    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;
};

// A D3D11 device with video support on the default adapter, standing in for the
// decoders' device. d3d11.dll is loaded at run time so the test links no D3D library.
struct D3DDevice {
    HMODULE library = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    uint64_t serial = 0;

    D3DDevice() {
        library = LoadLibraryW(L"d3d11.dll");
        if (!library) return;
        using Create = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                        UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        auto create = reinterpret_cast<Create>(GetProcAddress(library, "D3D11CreateDevice"));
        if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
            device = nullptr;
            context = nullptr;
            return;
        }
        // Multithread-protected, like the device FFmpeg creates for the decoders.
        ID3D10Multithread* multithread = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&multithread)))) {
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
        }
    }
    ~D3DDevice() {
        if (context) context->Release();
        if (device) device->Release();
        if (library) FreeLibrary(library);
    }
    D3DDevice(const D3DDevice&) = delete;
    D3DDevice& operator=(const D3DDevice&) = delete;
    bool ok() const { return device != nullptr; }
};

constexpr int BAR_WIDTH = 64;
constexpr int WIDTH = BAR_WIDTH * static_cast<int>(BARS.size());
constexpr int HEIGHT = 64;

// Colour bars in an NV12 or P010 texture (the decoders' pool format), as a GPU picture.
struct GpuBars {
    ID3D11Texture2D* texture = nullptr;
    YuvPicture picture;
    GpuBars() = default;
    GpuBars(const GpuBars&) = delete;
    GpuBars& operator=(const GpuBars&) = delete;
    ~GpuBars() {
        picture = YuvPicture{};
        if (texture) texture->Release();
    }
};

std::unique_ptr<GpuBars> makeGpuBars(D3DDevice& d3d, bool tenBit, YuvMatrix matrix, bool fullRange) {
    const int bits = tenBit ? 10 : 8;
    const int sample = tenBit ? 2 : 1;
    const int pitch = WIDTH * sample;
    // Y rows then interleaved CbCr rows, at one pitch: the initial-data layout of NV12/P010.
    std::vector<uint8_t> data(static_cast<size_t>(pitch) * (HEIGHT + HEIGHT / 2));
    auto store = [&](size_t offset, int value) {
        if (tenBit) {
            const uint16_t v = static_cast<uint16_t>(value << 6);
            std::memcpy(&data[offset], &v, 2);
        } else {
            data[offset] = static_cast<uint8_t>(value);
        }
    };
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            const Yuv c = toYuv(BARS[static_cast<size_t>(x / BAR_WIDTH)], matrix, fullRange, bits);
            store(static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * sample, c.y);
            if (x % 2 || y % 2) continue;
            const size_t row = static_cast<size_t>(HEIGHT + y / 2) * pitch;
            store(row + static_cast<size_t>(x) * sample, c.cb);
            store(row + static_cast<size_t>(x + 1) * sample, c.cr);
        }
    }

    auto bars = std::make_unique<GpuBars>();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = WIDTH;
    desc.Height = HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // Filled as a decoder fills its pool: the bars go into a plain texture, then are copied
    // into the shared one under its keyed mutex (initial data of a keyed-mutex texture is
    // not published to another device).
    const D3D11_SUBRESOURCE_DATA init{data.data(), static_cast<UINT>(pitch), 0};
    ID3D11Texture2D* source = nullptr;
    if (FAILED(d3d.device->CreateTexture2D(&desc, &init, &source))) return nullptr;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    HANDLE shared = nullptr;
    IDXGIResource* resource = nullptr;
    IDXGIKeyedMutex* mutex = nullptr;
    bool ok = SUCCEEDED(d3d.device->CreateTexture2D(&desc, nullptr, &bars->texture)) &&
              SUCCEEDED(bars->texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource))) &&
              SUCCEEDED(resource->GetSharedHandle(&shared)) &&
              SUCCEEDED(bars->texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutex))) &&
              mutex->AcquireSync(0, INFINITE) == S_OK;
    if (ok) {
        d3d.context->CopyResource(bars->texture, source);
        mutex->ReleaseSync(0);
    }
    if (mutex) mutex->Release();
    if (resource) resource->Release();
    source->Release();
    if (!ok) return nullptr;

    YuvPicture& pic = bars->picture;
    pic.layout = tenBit ? YuvLayout::P010 : YuvLayout::NV12;
    pic.width = WIDTH;
    pic.height = HEIGHT;
    pic.matrix = matrix;
    pic.fullRange = fullRange;
    pic.gpu.texture = bars->texture;
    pic.gpu.shared = shared;
    pic.gpu.serial = ++d3d.serial;
    pic.gpu.device = d3d.device;
    return bars;
}

std::vector<uint8_t> readBack(const Texture& texture) {
    std::vector<uint8_t> rgba(static_cast<size_t>(texture.getWidth()) * texture.getHeight() * 4);
    glBindTexture(GL_TEXTURE_2D, texture.getId());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return rgba;
}

const char* name(YuvMatrix matrix) {
    return matrix == YuvMatrix::BT601 ? "BT.601" : matrix == YuvMatrix::BT709 ? "BT.709" : "BT.2020";
}

// One frame through stage() and finish(); returns the failure reason, empty on success.
std::string deliver(DxInterop& interop, DxInteropTarget& target, Texture& dest, const YuvPicture& picture) {
    YuvConverter converter;
    std::string why;
    converter.begin();
    if (interop.stage(picture, target, dest, &dest, why)) {
        interop.finish(converter, [&](void*, const YuvPicture&, const std::string& reason) { why = reason; });
    }
    converter.end();
    return why;
}

// The fixtures a test needs, or the reason to skip.
struct Rig {
    GLContext gl;
    D3DDevice d3d;
    std::string skip;
    Rig() {
        DxInterop::testHooks() = DxInterop::TestHooks{};
        if (!gl.ok) skip = "no GL 3.3 context";
        else if (!d3d.ok()) skip = "no D3D11 video device";
    }
    ~Rig() { DxInterop::testHooks() = DxInterop::TestHooks{}; }
};

}  // namespace

TEST(DxInterop, ColourBarsMatchReferenceForEveryMatrixRangeAndBitDepth) {
    Rig rig;
    if (!rig.skip.empty()) GTEST_SKIP() << rig.skip;
    DxInterop interop;
    if (!interop.available()) GTEST_SKIP() << interop.unavailableReason();

    Texture dest;
    ASSERT_TRUE(dest.createEmpty(WIDTH, HEIGHT, 4));
    int worst = 0;
    for (bool tenBit : {false, true}) {
        for (YuvMatrix matrix : {YuvMatrix::BT601, YuvMatrix::BT709, YuvMatrix::BT2020}) {
            for (bool fullRange : {false, true}) {
                const std::string label = std::string(tenBit ? "P010 " : "NV12 ") + name(matrix) +
                                          (fullRange ? " full" : " limited");
                const auto bars = makeGpuBars(rig.d3d, tenBit, matrix, fullRange);
                ASSERT_NE(bars, nullptr) << label;
                // Each case gets a fresh target, as a source that fell back would.
                DxInteropTarget target;
                const std::string why = deliver(interop, target, dest, bars->picture);
                ASSERT_TRUE(why.empty()) << label << ": " << why;
                const std::vector<uint8_t> rgba = readBack(dest);
                int caseWorst = 0;
                for (size_t i = 0; i < BARS.size(); ++i) {
                    const size_t x = i * BAR_WIDTH + BAR_WIDTH / 2;
                    const uint8_t* px = &rgba[((HEIGHT / 2) * static_cast<size_t>(WIDTH) + x) * 4];
                    const Rgb got{px[0], px[1], px[2]};
                    const Rgb want = BARS[i];
                    caseWorst = std::max({caseWorst, std::abs(got.r - want.r), std::abs(got.g - want.g),
                                          std::abs(got.b - want.b)});
                    EXPECT_TRUE(within(got, want, 2))
                        << label << " bar " << i << ": got (" << got.r << "," << got.g << "," << got.b
                        << ") want (" << want.r << "," << want.g << "," << want.b << ")";
                }
                worst = std::max(worst, caseWorst);
                std::printf("[ colour ] zero-copy %s: worst channel error %d\n", label.c_str(), caseWorst);
            }
        }
    }
    RecordProperty("worst_channel_error", worst);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

// A target keeps showing each new frame: the shared texture is re-read through the
// interop lock every time, not captured once at registration.
TEST(DxInterop, EveryNewFrameReachesTheTexture) {
    Rig rig;
    if (!rig.skip.empty()) GTEST_SKIP() << rig.skip;
    DxInterop interop;
    if (!interop.available()) GTEST_SKIP() << interop.unavailableReason();

    Texture dest;
    ASSERT_TRUE(dest.createEmpty(WIDTH, HEIGHT, 4));
    DxInteropTarget target;
    // Limited then full range of the same samples: white's luma (235) reads 255, then 235.
    const auto limited = makeGpuBars(rig.d3d, false, YuvMatrix::BT709, false);
    ASSERT_NE(limited, nullptr);
    ASSERT_EQ(deliver(interop, target, dest, limited->picture), "");
    const uint8_t first = readBack(dest)[((HEIGHT / 2) * static_cast<size_t>(WIDTH) + BAR_WIDTH / 2) * 4];
    YuvPicture relabelled = limited->picture;
    relabelled.fullRange = true;
    ASSERT_EQ(deliver(interop, target, dest, relabelled), "");
    const uint8_t second = readBack(dest)[((HEIGHT / 2) * static_cast<size_t>(WIDTH) + BAR_WIDTH / 2) * 4];
    EXPECT_NEAR(first, 255, 2);
    EXPECT_NEAR(second, 235, 2);
}

TEST(DxInterop, EachForcedFallbackIsReported) {
    Rig rig;
    if (!rig.skip.empty()) GTEST_SKIP() << rig.skip;
    {
        DxInterop probe;
        if (!probe.available()) GTEST_SKIP() << probe.unavailableReason();
    }
    Texture dest;
    ASSERT_TRUE(dest.createEmpty(WIDTH, HEIGHT, 4));
    const auto bars = makeGpuBars(rig.d3d, false, YuvMatrix::BT709, false);
    ASSERT_NE(bars, nullptr);

    auto run = [&](const DxInterop::TestHooks& hooks) {
        DxInterop::testHooks() = hooks;
        DxInterop interop;
        DxInteropTarget target;
        return deliver(interop, target, dest, bars->picture);
    };
    DxInterop::TestHooks hooks;
    EXPECT_EQ(run(hooks), "");  // the path works when nothing is forced

    hooks = {};
    hooks.extensionMissing = true;
    EXPECT_NE(run(hooks).find("WGL_NV_DX_interop2"), std::string::npos);
    hooks = {};
    hooks.failRegistration = true;
    EXPECT_NE(run(hooks).find("register"), std::string::npos);
    hooks = {};
    hooks.rejectFormat = true;
    EXPECT_NE(run(hooks).find("pixel format"), std::string::npos);
    hooks = {};
    hooks.failLockAfter = 0;
    EXPECT_NE(run(hooks).find("lock"), std::string::npos);
}

TEST(DxInterop, ReleasesEveryObjectItMade) {
    Rig rig;
    if (!rig.skip.empty()) GTEST_SKIP() << rig.skip;
    const long d3dBefore = DxInterop::liveObjects();
    const auto glBefore = GlObjectStats::Snapshot::capture();
    {
        DxInterop interop;
        if (!interop.available()) GTEST_SKIP() << interop.unavailableReason();
        Texture dest;
        ASSERT_TRUE(dest.createEmpty(WIDTH, HEIGHT, 4));
        for (int source = 0; source < 3; ++source) {  // churn: a target per source
            DxInteropTarget target;
            for (int frame = 0; frame < 20; ++frame) {  // more frames than the view cache holds
                const auto bars = makeGpuBars(rig.d3d, frame % 2 == 1, YuvMatrix::BT709, false);
                ASSERT_NE(bars, nullptr);
                ASSERT_EQ(deliver(interop, target, dest, bars->picture), "");
            }
            EXPECT_GT(DxInterop::liveObjects(), d3dBefore);
        }
        EXPECT_EQ(DxInterop::liveObjects(), d3dBefore) << "every target gone: nothing held for sources";
    }
    EXPECT_EQ(DxInterop::liveObjects(), d3dBefore);
    EXPECT_TRUE(GlObjectStats::Snapshot::capture().isFlat(glBefore))
        << GlObjectStats::Snapshot::capture().diff(glBefore);
}
