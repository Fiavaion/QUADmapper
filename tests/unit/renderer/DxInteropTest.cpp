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
#include "core/MainThreadTimings.hpp"
#include "renderer/DxInterop.hpp"
#include "renderer/Texture.hpp"
#include "renderer/YuvConverter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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

// NV12 or P010 samples: Y rows then interleaved CbCr rows at one pitch (the initial-data
// layout of those formats). `colour(x, y)` gives each pixel's Y'CbCr; chroma is taken at
// even pixels.
template <typename Colour>
std::vector<uint8_t> planeData(bool tenBit, Colour colour) {
    const int sample = tenBit ? 2 : 1;
    const int pitch = WIDTH * sample;
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
            const Yuv c = colour(x, y);
            store(static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * sample, c.y);
            if (x % 2 || y % 2) continue;
            const size_t row = static_cast<size_t>(HEIGHT + y / 2) * pitch;
            store(row + static_cast<size_t>(x) * sample, c.cb);
            store(row + static_cast<size_t>(x + 1) * sample, c.cr);
        }
    }
    return data;
}

// The same samples as a picture in memory, for the readback path's conversion.
YuvPicture memoryPicture(const std::vector<uint8_t>& data, bool tenBit, YuvMatrix matrix, bool fullRange) {
    const int pitch = WIDTH * (tenBit ? 2 : 1);
    YuvPicture pic;
    pic.layout = tenBit ? YuvLayout::P010 : YuvLayout::NV12;
    pic.width = WIDTH;
    pic.height = HEIGHT;
    pic.planes[0] = data.data();
    pic.planes[1] = data.data() + static_cast<size_t>(pitch) * HEIGHT;
    pic.strides[0] = pic.strides[1] = pitch;
    pic.matrix = matrix;
    pic.fullRange = fullRange;
    return pic;
}

// Fill a shared frame texture as a decoder fills its pool: the samples go into a plain
// texture, then are copied into the shared one under its keyed mutex (initial data of a
// keyed-mutex texture is not published to another device).
bool fillGpuFrame(D3DDevice& d3d, GpuBars& frame, const std::vector<uint8_t>& data, bool tenBit) {
    D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    const D3D11_SUBRESOURCE_DATA init{data.data(), static_cast<UINT>(WIDTH * (tenBit ? 2 : 1)), 0};
    ID3D11Texture2D* source = nullptr;
    if (FAILED(d3d.device->CreateTexture2D(&desc, &init, &source))) return false;
    IDXGIKeyedMutex* mutex = nullptr;
    bool ok = SUCCEEDED(frame.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutex))) &&
              mutex->AcquireSync(0, INFINITE) == S_OK;
    if (ok) {
        d3d.context->CopyResource(frame.texture, source);
        mutex->ReleaseSync(0);
        d3d.context->Flush();
    }
    if (mutex) mutex->Release();
    source->Release();
    return ok;
}

// An NV12 or P010 texture shared with a keyed mutex (the decoders' pool format), as a GPU
// picture of `pool`.
std::unique_ptr<GpuBars> makeGpuFrame(D3DDevice& d3d, bool tenBit, YuvMatrix matrix, bool fullRange,
                                      uint64_t pool = 0) {
    auto frame = std::make_unique<GpuBars>();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = WIDTH;
    desc.Height = HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    HANDLE shared = nullptr;
    IDXGIResource* resource = nullptr;
    const bool ok = SUCCEEDED(d3d.device->CreateTexture2D(&desc, nullptr, &frame->texture)) &&
                    SUCCEEDED(frame->texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource))) &&
                    SUCCEEDED(resource->GetSharedHandle(&shared));
    if (resource) resource->Release();
    if (!ok) return nullptr;

    YuvPicture& pic = frame->picture;
    pic.layout = tenBit ? YuvLayout::P010 : YuvLayout::NV12;
    pic.width = WIDTH;
    pic.height = HEIGHT;
    pic.matrix = matrix;
    pic.fullRange = fullRange;
    pic.gpu.texture = frame->texture;
    pic.gpu.shared = shared;
    pic.gpu.serial = ++d3d.serial;
    pic.gpu.pool = pool;
    pic.gpu.device = d3d.device;
    return frame;
}

std::vector<uint8_t> barsData(bool tenBit, YuvMatrix matrix, bool fullRange) {
    const int bits = tenBit ? 10 : 8;
    return planeData(tenBit, [&](int x, int) {
        return toYuv(BARS[static_cast<size_t>(x / BAR_WIDTH)], matrix, fullRange, bits);
    });
}

// Colour bars in an NV12 or P010 texture (the decoders' pool format), as a GPU picture.
std::unique_ptr<GpuBars> makeGpuBars(D3DDevice& d3d, bool tenBit, YuvMatrix matrix, bool fullRange) {
    auto bars = makeGpuFrame(d3d, tenBit, matrix, fullRange);
    if (!bars || !fillGpuFrame(d3d, *bars, barsData(tenBit, matrix, fullRange), tenBit)) return nullptr;
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

// RGB channels only (alpha is 255 on every path): the largest difference.
int worstDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    int worst = a.size() == b.size() ? 0 : 255;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (i % 4 != 3) worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return worst;
}

// `picture` (in memory) through the readback path's conversion.
std::vector<uint8_t> referenceRgba(const YuvPicture& picture) {
    Texture reference;
    if (!reference.createEmpty(WIDTH, HEIGHT, 4)) return {};
    YuvConverter converter;
    YuvUploadState state;
    converter.begin();
    const bool ok = converter.convert(picture, reference, state);
    converter.end();
    return ok ? readBack(reference) : std::vector<uint8_t>{};
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
    hooks = {};
    hooks.failLockDest = &dest;
    EXPECT_NE(run(hooks).find("lock"), std::string::npos);
    hooks = {};
    hooks.adapterMismatch = true;
    EXPECT_NE(run(hooks).find("another GPU"), std::string::npos);
}

// Several sources share one interop lock per frame. When one source's planes cannot be
// locked, only that source falls back: the others are locked on their own and still
// get their own frame (the lock used to fail for all of them together).
TEST(DxInterop, OneSourcesLockFailureDemotesOnlyThatSource) {
    Rig rig;
    if (!rig.skip.empty()) GTEST_SKIP() << rig.skip;
    DxInterop interop;
    if (!interop.available()) GTEST_SKIP() << interop.unavailableReason();

    constexpr int SOURCES = 4;
    constexpr int BAD = 2;
    std::vector<std::unique_ptr<GpuBars>> frames;
    std::vector<std::vector<uint8_t>> samples;
    std::vector<std::unique_ptr<Texture>> dests;
    std::vector<std::unique_ptr<DxInteropTarget>> targets;
    for (int i = 0; i < SOURCES; ++i) {
        // A different flat colour per source, so a frame delivered to the wrong texture shows.
        samples.push_back(planeData(false, [i](int, int) { return Yuv{40 + 40 * i, 90 + 20 * i, 160 - 15 * i}; }));
        frames.push_back(makeGpuFrame(rig.d3d, false, YuvMatrix::BT709, false));
        ASSERT_NE(frames.back(), nullptr);
        ASSERT_TRUE(fillGpuFrame(rig.d3d, *frames.back(), samples.back(), false));
        dests.push_back(std::make_unique<Texture>());
        ASSERT_TRUE(dests.back()->createEmpty(WIDTH, HEIGHT, 4));
        targets.push_back(std::make_unique<DxInteropTarget>());
    }
    DxInterop::testHooks().failLockDest = dests[BAD].get();

    YuvConverter converter;
    std::vector<void*> failedKeys;
    converter.begin();
    for (int i = 0; i < SOURCES; ++i) {
        std::string why;
        ASSERT_TRUE(interop.stage(frames[i]->picture, *targets[i], *dests[i], dests[i].get(), why)) << why;
    }
    interop.finish(converter, [&](void* key, const YuvPicture&, const std::string& reason) {
        EXPECT_NE(reason.find("lock"), std::string::npos) << reason;
        failedKeys.push_back(key);
    });
    converter.end();

    ASSERT_EQ(failedKeys.size(), 1u) << "only the source whose lock fails falls back";
    EXPECT_EQ(failedKeys[0], dests[BAD].get());
    for (int i = 0; i < SOURCES; ++i) {
        if (i == BAD) continue;
        const std::vector<uint8_t> want = referenceRgba(memoryPicture(samples[i], false, YuvMatrix::BT709, false));
        EXPECT_LE(worstDifference(readBack(*dests[i]), want), 1) << "source " << i;
    }
}

// Decoders reuse a pool of shared textures round-robin, and the renderer keeps each one
// opened. Every frame must still reach the texture with its own content: distinct
// frames go through one target across more than one wrap of a 12-texture pool, and each
// result is compared with the same samples converted from memory. (Skipping the copy for
// a texture already opened would freeze or loop the picture.)
TEST(DxInterop, DistinctFramesThroughOneTargetAcrossPoolWraps) {
    Rig rig;
    if (!rig.skip.empty()) GTEST_SKIP() << rig.skip;
    DxInterop interop;
    if (!interop.available()) GTEST_SKIP() << interop.unavailableReason();

    constexpr int POOL = 12;
    constexpr int FRAMES = 30;
    for (bool tenBit : {false, true}) {
        const int scale = tenBit ? 4 : 1;
        std::vector<std::unique_ptr<GpuBars>> pool;
        for (int i = 0; i < POOL; ++i) {
            pool.push_back(makeGpuFrame(rig.d3d, tenBit, YuvMatrix::BT709, false, 1));
            ASSERT_NE(pool.back(), nullptr);
        }
        Texture dest;
        ASSERT_TRUE(dest.createEmpty(WIDTH, HEIGHT, 4));
        DxInteropTarget target;
        for (int f = 0; f < FRAMES; ++f) {
            // A gradient that moves with the frame number, so no two frames match.
            const std::vector<uint8_t> samples = planeData(tenBit, [&](int x, int y) {
                return Yuv{(20 + (x + 7 * f) % 200) * scale, (60 + (y * 3 + 5 * f) % 120) * scale,
                           (200 - (x / 2 + 3 * f) % 120) * scale};
            });
            GpuBars& frame = *pool[static_cast<size_t>(f % POOL)];
            ASSERT_TRUE(fillGpuFrame(rig.d3d, frame, samples, tenBit));
            ASSERT_EQ(deliver(interop, target, dest, frame.picture), "") << "frame " << f;
            const std::vector<uint8_t> want = referenceRgba(memoryPicture(samples, tenBit, YuvMatrix::BT709, false));
            ASSERT_LE(worstDifference(readBack(dest), want), 1)
                << (tenBit ? "P010" : "NV12") << " frame " << f << " (pool texture " << f % POOL
                << ") is not the frame delivered";
        }
    }
}

// A decoder that makes a new pool (a new stream, a size change) never sends the old
// pool's textures again: the target lets go of them at the first frame of the new pool
// instead of keeping them, and their video memory, open.
TEST(DxInterop, ANewDecoderPoolReleasesTheOldPoolsFrames) {
    Rig rig;
    if (!rig.skip.empty()) GTEST_SKIP() << rig.skip;
    DxInterop interop;
    if (!interop.available()) GTEST_SKIP() << interop.unavailableReason();

    Texture dest;
    ASSERT_TRUE(dest.createEmpty(WIDTH, HEIGHT, 4));
    DxInteropTarget target;
    const std::vector<uint8_t> samples = barsData(false, YuvMatrix::BT709, false);
    std::vector<std::unique_ptr<GpuBars>> frames;
    long oneFrameOpen = 0;
    for (int i = 0; i < 4; ++i) {
        frames.push_back(makeGpuFrame(rig.d3d, false, YuvMatrix::BT709, false, 1));
        ASSERT_NE(frames.back(), nullptr);
        ASSERT_TRUE(fillGpuFrame(rig.d3d, *frames.back(), samples, false));
        ASSERT_EQ(deliver(interop, target, dest, frames.back()->picture), "");
        if (i == 0) oneFrameOpen = DxInterop::liveObjects();
    }
    EXPECT_GT(DxInterop::liveObjects(), oneFrameOpen) << "the pool's frames stay opened while it is in use";

    auto next = makeGpuFrame(rig.d3d, false, YuvMatrix::BT709, false, 2);
    ASSERT_NE(next, nullptr);
    ASSERT_TRUE(fillGpuFrame(rig.d3d, *next, samples, false));
    ASSERT_EQ(deliver(interop, target, dest, next->picture), "");
    EXPECT_EQ(DxInterop::liveObjects(), oneFrameOpen) << "only the new pool's one frame is open";
}

// TD-35: a source's first zero-copy frame is set up over several updates, with at most one
// costly GL call in each: the device prepared off the main thread is opened for OpenGL by
// beginFrame(), the shared planes' D3D11 textures are made on a worker, then each plane is
// registered in an update of its own. Once Ready, the frame is delivered as stage() alone
// would deliver it, and the shared planes are freed a plane a step.
TEST(DxInterop, SetUpSpreadsTheFirstFrameOverUpdatesOneGlCallEach) {
    Rig rig;
    if (!rig.skip.empty()) GTEST_SKIP() << rig.skip;
    const long d3dBefore = DxInterop::liveObjects();
    const auto glBefore = GlObjectStats::Snapshot::capture();
    MainThreadTimings::markMainThread();
    MainThreadTimings::reset();
    auto glCalls = [] {
        size_t n = 0;
        for (const auto& s : MainThreadTimings::summarize()) {
            if (s.op == "zeroCopyDeviceOpen" || s.op == "zeroCopyPlaneRegister") n += s.count;
        }
        return n;
    };
    {
        DxInterop interop;
        if (!interop.available()) GTEST_SKIP() << interop.unavailableReason();
        interop.prepare(rig.d3d.device);  // as the opener does
        const auto bars = makeGpuBars(rig.d3d, false, YuvMatrix::BT709, false);
        ASSERT_NE(bars, nullptr);
        Texture dest;
        ASSERT_TRUE(dest.createEmpty(WIDTH, HEIGHT, 4));
        auto target = std::make_unique<DxInteropTarget>();

        interop.beginFrame();
        EXPECT_EQ(glCalls(), 1u) << "beginFrame() did not open the prepared device";
        int updates = 0;
        DxInterop::SetUp result = DxInterop::SetUp::Later;
        std::string why;
        for (; updates < 5000 && result == DxInterop::SetUp::Later; ++updates) {
            if (updates > 0) interop.beginFrame();
            const size_t before = glCalls();
            result = interop.setUp(bars->picture, *target, why);
            EXPECT_LE(glCalls() - before + (updates == 0 ? 1 : 0), 1u) << "update " << updates;
            if (result == DxInterop::SetUp::Later) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(result, DxInterop::SetUp::Ready) << why;
        EXPECT_GE(updates, 3) << "the planes were registered in one update";
        EXPECT_EQ(glCalls(), 3u);  // the device open and one registration per plane

        YuvConverter converter;
        converter.begin();
        ASSERT_TRUE(interop.stage(bars->picture, *target, dest, &dest, why)) << why;
        why.clear();
        interop.finish(converter, [&](void*, const YuvPicture&, const std::string& reason) { why = reason; });
        converter.end();
        ASSERT_TRUE(why.empty()) << why;
        EXPECT_LE(worstDifference(readBack(dest), referenceRgba(memoryPicture(
                                                        barsData(false, YuvMatrix::BT709, false), false,
                                                        YuvMatrix::BT709, false))),
                  2);

        // Freed a plane a step: GL names go one at a time.
        const auto held = GlObjectStats::Snapshot::capture();
        EXPECT_FALSE(target->releaseStep());
        EXPECT_EQ(GlObjectStats::Snapshot::capture().textures, held.textures - 1);
        EXPECT_TRUE(target->releaseStep());
        EXPECT_EQ(GlObjectStats::Snapshot::capture().textures, held.textures - 2);
        target.reset();
        EXPECT_EQ(DxInterop::liveObjects(), d3dBefore);
    }
    MainThreadTimings::reset();
    EXPECT_EQ(DxInterop::liveObjects(), d3dBefore);
    EXPECT_TRUE(GlObjectStats::Snapshot::capture().isFlat(glBefore))
        << GlObjectStats::Snapshot::capture().diff(glBefore);
}

// Each failure setUp() can meet is reported, not left Later forever: a format that
// cannot be shared (found on the worker) and a registration refused.
TEST(DxInterop, SetUpReportsEachFailure) {
    Rig rig;
    if (!rig.skip.empty()) GTEST_SKIP() << rig.skip;
    const long d3dBefore = DxInterop::liveObjects();
    for (const bool rejectFormat : {true, false}) {
        DxInterop::testHooks() = DxInterop::TestHooks{};
        DxInterop::testHooks().rejectFormat = rejectFormat;
        DxInterop::testHooks().failRegistration = !rejectFormat;
        DxInterop interop;
        if (!interop.available()) GTEST_SKIP() << interop.unavailableReason();
        const auto bars = makeGpuBars(rig.d3d, true, YuvMatrix::BT2020, false);
        ASSERT_NE(bars, nullptr);
        DxInteropTarget target;
        DxInterop::SetUp result = DxInterop::SetUp::Later;
        std::string why;
        for (int updates = 0; updates < 5000 && result == DxInterop::SetUp::Later; ++updates) {
            interop.beginFrame();
            result = interop.setUp(bars->picture, target, why);
            if (result == DxInterop::SetUp::Later) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(result, DxInterop::SetUp::Failed);
        EXPECT_NE(why.find(rejectFormat ? "cannot be shared" : "could not register"), std::string::npos) << why;
    }
    DxInterop::testHooks() = DxInterop::TestHooks{};
    EXPECT_EQ(DxInterop::liveObjects(), d3dBefore);
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

// ADR-056 hybrid-GPU check: GL names the adapter it renders on, even when an earlier GL
// call left an error pending (in the app one always was, so the check never ran), and
// that adapter is one DXGI lists.
TEST(DxInterop, GlNamesItsAdapterDespiteAPendingError) {
    GLContext gl;
    if (!gl.ok) GTEST_SKIP() << "no GL 3.3 context";
    if (!glfwExtensionSupported("GL_EXT_memory_object_win32") && !glfwExtensionSupported("GL_EXT_semaphore_win32")) {
        GTEST_SKIP() << "the driver does not name its GPU";
    }
    glEnable(0xFFFF);  // GL_INVALID_ENUM, left pending
    uint64_t luid = 0;
    std::string why;
    ASSERT_TRUE(DxInterop::glAdapterLuid(luid, why)) << why;

    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1* factory = nullptr;
    ASSERT_TRUE(create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))));
    bool listed = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        uint64_t adapterLuid = 0;
        std::memcpy(&adapterLuid, &desc.AdapterLuid, sizeof(adapterLuid));
        listed = listed || adapterLuid == luid;
        adapter->Release();
    }
    factory->Release();
    FreeLibrary(dxgi);
    EXPECT_TRUE(listed) << "GL's adapter " << luid << " is not one DXGI lists";
}

