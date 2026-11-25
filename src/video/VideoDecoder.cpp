#include "VideoDecoder.hpp"
#include "DecodeBackoff.hpp"
#include "FrameSelection.hpp"
#include "PlaybackClock.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "core/MediaLimits.hpp"

#ifdef QUADMAPPER_HAS_FFMPEG

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// C++ headers: included here, outside extern "C", before hwcontext_d3d11va.h pulls them in.
#include <d3d11_4.h>  // D3D11_FEATURE_DATA_D3D11_OPTIONS4
#include <dxgi.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
#ifdef QUADMAPPER_HAS_AVDEVICE
#include <libavdevice/avdevice.h>
#endif
}
#include "LocalMediaInput.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace quadmapper {

namespace {

constexpr size_t MEGABYTE = size_t(1) << 20;
// Seeks further back tried when a reverse step finds nothing before its bound.
constexpr int MAX_REVERSE_RETRIES = 20;
// Leg changes in a row that produced no frame: the plan cannot make progress.
constexpr int MAX_EMPTY_LEGS = 3;
// Frames decoded while a Loop splice is pending before the seam is forced by count
// instead of by pts: comfortably past any real B-frame reorder depth, so a stream that
// drops a timestamp right at the seam cannot wedge the splice forever (Finding 2).
constexpr int MAX_SPLICE_PENDING_FRAMES = 32;
// Row alignment of frames copied from the GPU.
constexpr int TRANSFER_ALIGN = 32;
// How long a wait for a GPU copy may take before the copy counts as failed. Polled
// every half millisecond (in practice a Windows sleep lasts up to a timer tick).
constexpr std::chrono::seconds GPU_COPY_TIMEOUT{2};

size_t frameBytes(const AVFrame* frame) {
    size_t bytes = 0;
    for (const AVBufferRef* buf : frame->buf) {
        if (buf) bytes += buf->size;
    }
    return bytes;
}

std::string formatBytes(double bytes) {
    if (bytes >= static_cast<double>(MEGABYTE)) {
        return std::to_string(static_cast<int64_t>(bytes / MEGABYTE)) + " MB";
    }
    return std::to_string(static_cast<int64_t>(bytes / 1024.0)) + " KB";
}

std::string reverseAdvice(size_t cacheBytes) {
    return "more than the " + formatBytes(static_cast<double>(cacheBytes)) +
           " reverse cache. Re-encode as HAP or all-intra for Ping-Pong.";
}

// --- Hardware decoding (ADR-055) ---

// The D3D11 device every hardware decoder shares, one per app. Created by the first
// decoder that asks and then kept until the process ends: creating and destroying a
// D3D11 device per clip costs ~1.5 MB of driver memory each time that is never given
// back (40 stress cycles re-creating a clip: +65 MB, against +25 MB with one device).
// Not released at exit either: freeing D3D objects from a static destructor races the
// driver's own unload, and the OS reclaims the device with the process.
struct SharedHardwareDevice {
    std::mutex mutex;
    AVBufferRef* device = nullptr;  // the registry's reference, held until exit
    int created = 0;
    bool unavailable = false;       // creation failed once: don't retry per clip
};

SharedHardwareDevice& sharedHardwareDevice() {
    static SharedHardwareDevice shared;
    return shared;
}

std::string adapterName(AVBufferRef* device) {
#ifdef _WIN32
    const auto* hw = reinterpret_cast<const AVHWDeviceContext*>(device->data);
    const auto* d3d = static_cast<const AVD3D11VADeviceContext*>(hw->hwctx);
    IDXGIDevice* dxgiDevice = nullptr;
    if (!d3d || !d3d->device ||
        FAILED(d3d->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
        return "unknown adapter";
    }
    std::string name = "unknown adapter";
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
        name.clear();
        for (const wchar_t* c = desc.Description; *c; ++c) {
            name += (*c < 128) ? static_cast<char>(*c) : '?';
        }
    }
    if (adapter) adapter->Release();
    dxgiDevice->Release();
    return name;
#else
    (void)device;
    return "unknown adapter";
#endif
}

// A new reference to the shared device, or null when the machine has none.
AVBufferRef* acquireHardwareDevice() {
    SharedHardwareDevice& shared = sharedHardwareDevice();
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (shared.unavailable) return nullptr;
    if (!shared.device) {
        AVBufferRef* device = nullptr;
        const int rc = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (rc < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(rc, err, sizeof(err));
            LOG_WARNING("Hardware video decoding unavailable (no D3D11 video device: {}); "
                        "videos decode in software", err);
            shared.unavailable = true;
            return nullptr;
        }
        LOG_INFO("Hardware video decoding: D3D11VA on {}", adapterName(device));
        shared.device = device;
        ++shared.created;
    }
    return av_buffer_ref(shared.device);
}

// The codecs the GPU pass is asked to decode (ADR-055 §1).
bool hardwareCodec(AVCodecID id) {
    return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC || id == AV_CODEC_ID_VP9 ||
           id == AV_CODEC_ID_AV1;
}

// The decoder for `id` that can use D3D11VA (for AV1 the native one, not libdav1d), or null.
const AVCodec* hardwareDecoder(AVCodecID id) {
    const AVCodec* codec = id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("av1")
                                                  : avcodec_find_decoder(id);
    for (int i = 0; codec; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;
        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA && config->pix_fmt == AV_PIX_FMT_D3D11 &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            return codec;
        }
    }
    return nullptr;
}

// get_format: take the D3D11 surfaces when offered; otherwise (the GPU refused this
// stream's profile or size) the first software format. The decoder notices the
// software frames and restarts the stream in proper, multi-threaded software decoding.
AVPixelFormat chooseHardwareFormat(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_D3D11) return *f;
    }
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*f);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
    }
    return AV_PIX_FMT_NONE;
}

void freeFrameHandle(AVFrame* frame) {
    av_frame_free(&frame);
}

#ifdef _WIN32
// The shared device's D3D11 interfaces and lock, or null before the first hardware decoder.
AVD3D11VADeviceContext* sharedD3d() {
    SharedHardwareDevice& shared = sharedHardwareDevice();
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (!shared.device) return nullptr;
    const auto* hw = reinterpret_cast<const AVHWDeviceContext*>(shared.device->data);
    return static_cast<AVD3D11VADeviceContext*>(hw->hwctx);
}

AVPixelFormat memoryFormatOf(ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    switch (desc.Format) {
        case DXGI_FORMAT_NV12: return AV_PIX_FMT_NV12;
        case DXGI_FORMAT_P010: return AV_PIX_FMT_P010LE;
        default: return AV_PIX_FMT_NONE;
    }
}

void releaseTexture(ID3D11Texture2D*& texture) {
    if (texture) texture->Release();
    texture = nullptr;
}

// Decoders' D3D11 objects other than GPU frames: readback staging textures and copy
// event queries (VideoDecoder::liveD3dObjects).
std::atomic<long> g_liveD3dObjects{0};
// GPU-to-memory readbacks made by each thread (VideoDecoder::gpuReadbacksOnThisThread).
thread_local long t_gpuReadbacks = 0;

void releaseStaging(ID3D11Texture2D*& staging) {
    if (staging) g_liveD3dObjects.fetch_sub(1, std::memory_order_relaxed);
    releaseTexture(staging);
}

// Wait for `done()` to report the GPU work finished. False on failure, after
// GPU_COPY_TIMEOUT, or once `stop` is set (the decode thread is being joined, for a
// seek or shutdown, so it never waits on a hung GPU).
template <typename Done>
bool waitForGpu(Done done, const std::atomic<bool>* stop) {
    const auto deadline = std::chrono::steady_clock::now() + GPU_COPY_TIMEOUT;
    for (;;) {
        const int state = done();  // 1 done, 0 not yet, -1 failed
        if (state != 0) return state > 0;
        if ((stop && stop->load()) || std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

// The longest a decoder waits for a shared texture's keyed mutex. The wait is made with
// the decoders' device lock held, so one that never ended would stop every stream; past
// this the frame takes the caller's fallback (copied to memory, or skipped).
constexpr DWORD KEYED_MUTEX_WAIT_MS = 1000;

// Copy `texture` (array slice `index`) into `copy`, whose format, size and buffers are
// set, through `staging` (re-created when it does not fit). av_hwframe_transfer_data
// for D3D11 holds the shared device's lock while it waits for the GPU copy and while it
// copies the pixels, so with many streams every decoder queues behind every other
// (16 x 1080p: ~20 ms a frame). Here the lock is held only for the D3D11 calls
// themselves: copy to the staging texture, then poll the map without waiting, and copy
// the pixels out unlocked. `mutex` is the keyed mutex of a shared pool texture (ADR-056),
// held around the copy; null for a decoder surface.
int readBackTexture(AVD3D11VADeviceContext* d3d, ID3D11Texture2D* texture, UINT index,
                    ID3D11Texture2D*& staging, D3D11_TEXTURE2D_DESC& stagingDesc, AVFrame* copy,
                    IDXGIKeyedMutex* mutex, const std::atomic<bool>* stop) {
    ++t_gpuReadbacks;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!staging || stagingDesc.Width != desc.Width || stagingDesc.Height != desc.Height ||
        stagingDesc.Format != desc.Format) {
        releaseStaging(staging);
        D3D11_TEXTURE2D_DESC wanted{};
        wanted.Width = desc.Width;
        wanted.Height = desc.Height;
        wanted.MipLevels = 1;
        wanted.ArraySize = 1;
        wanted.Format = desc.Format;
        wanted.SampleDesc.Count = 1;
        wanted.Usage = D3D11_USAGE_STAGING;
        wanted.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d3d->device->CreateTexture2D(&wanted, nullptr, &staging))) {
            staging = nullptr;
            return AVERROR(ENOMEM);
        }
        g_liveD3dObjects.fetch_add(1, std::memory_order_relaxed);
        stagingDesc = wanted;
    }

    d3d->lock(d3d->lock_ctx);
    if (mutex && mutex->AcquireSync(0, KEYED_MUTEX_WAIT_MS) != S_OK) {
        d3d->unlock(d3d->lock_ctx);
        return AVERROR_EXTERNAL;
    }
    d3d->device_context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, index, nullptr);
    if (mutex) mutex->ReleaseSync(0);
    d3d->device_context->Flush();  // start the copy now: the map below does not wait
    d3d->unlock(d3d->lock_ctx);

    D3D11_MAPPED_SUBRESOURCE map{};
    const bool mapped = waitForGpu([&] {
        d3d->lock(d3d->lock_ctx);
        const HRESULT hr = d3d->device_context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
        d3d->unlock(d3d->lock_ctx);
        return SUCCEEDED(hr) ? 1 : hr == DXGI_ERROR_WAS_STILL_DRAWING ? 0 : -1;
    }, stop);
    if (!mapped) return stop && stop->load() ? AVERROR_EXIT : AVERROR_EXTERNAL;

    // The staging texture holds the surface at its padded height; the planes follow
    // each other at the map's row pitch.
    const auto format = static_cast<AVPixelFormat>(copy->format);
    uint8_t* src[4] = {};
    int srcLinesize[4] = {static_cast<int>(map.RowPitch), static_cast<int>(map.RowPitch), 0, 0};
    const int filled = av_image_fill_pointers(src, format, static_cast<int>(desc.Height),
                                              static_cast<uint8_t*>(map.pData), srcLinesize);
    if (filled >= 0) {
        av_image_copy(copy->data, copy->linesize, const_cast<const uint8_t**>(src), srcLinesize, format,
                      copy->width, copy->height);
    }
    d3d->lock(d3d->lock_ctx);
    d3d->device_context->Unmap(staging, 0);
    d3d->unlock(d3d->lock_ctx);
    return filled < 0 ? filled : 0;
}

// --- Frames left on the GPU (ADR-056) ---

// Textures one stream may hold frames in: the queued frames (8), the one shown, the one
// being decoded, and slack. Past this a frame is copied to memory instead, so a
// Ping-Pong cache or a stalled consumer cannot fill video memory.
constexpr int GPU_POOL_TEXTURES = 12;

std::atomic<long> g_liveGpuFrames{0};
std::atomic<uint64_t> g_gpuFrameSerial{0};

// One texture of a stream's pool: the data of the pool's AVBufferRefs. It is shared with
// the renderer's own D3D11 device (ADR-056), so every use on either device holds its
// keyed mutex (key 0: plain mutual exclusion, and the GPU ordering between the devices).
struct GpuFrameTexture {
    ID3D11Texture2D* texture = nullptr;
    IDXGIKeyedMutex* mutex = nullptr;
    HANDLE shared = nullptr;  // for OpenSharedResource; owned by the texture
    uint64_t serial = 0;
    uint64_t pool = 0;        // GpuPoolSpec::id
};

std::atomic<uint64_t> g_gpuPoolSerial{0};

// What a stream's pool makes its textures from. av_buffer_pool_get() calls the
// allocator under the pool's own lock, so `created` needs no other.
struct GpuPoolSpec {
    ID3D11Device* device = nullptr;  // the shared device (kept until exit)
    D3D11_TEXTURE2D_DESC desc{};
    uint64_t id = 0;                 // names the pool for the life of the process
    int created = 0;
    int failAfter = -1;              // DecodeOptions::failGpuTextureAfter
    bool failed = false;             // a texture could not be made (not the cap)
};

// Whether textures of `format` can be shared with a keyed mutex (ADR-056). NV12 needs
// the extended NV12 sharing of D3D11.4 (OPTIONS4); every format must be shareable.
bool canShareFormat(ID3D11Device* device, DXGI_FORMAT format) {
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{format, 0};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support))) ||
        !(support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_SHAREABLE)) {
        return false;
    }
    if (format != DXGI_FORMAT_NV12) return true;
    D3D11_FEATURE_DATA_D3D11_OPTIONS4 options{};
    return SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS4, &options, sizeof(options))) &&
           options.ExtendedNV12SharedTextureSupported;
}

void freeGpuFrameTexture(void*, uint8_t* data) {
    auto* entry = reinterpret_cast<GpuFrameTexture*>(data);
    if (entry->mutex) entry->mutex->Release();
    releaseTexture(entry->texture);
    g_liveGpuFrames.fetch_sub(1, std::memory_order_relaxed);
    delete entry;
}

AVBufferRef* allocGpuFrameTexture(void* opaque, size_t size) {
    auto* spec = static_cast<GpuPoolSpec*>(opaque);
    if (spec->created >= GPU_POOL_TEXTURES) return nullptr;
    auto* entry = new GpuFrameTexture;
    const bool injected = spec->failAfter >= 0 && spec->created >= spec->failAfter;
    if (injected || FAILED(spec->device->CreateTexture2D(&spec->desc, nullptr, &entry->texture))) {
        delete entry;
        spec->failed = true;
        return nullptr;
    }
    g_liveGpuFrames.fetch_add(1, std::memory_order_relaxed);
    IDXGIResource* resource = nullptr;
    const bool shared =
        SUCCEEDED(entry->texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&entry->mutex))) &&
        SUCCEEDED(entry->texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource))) &&
        SUCCEEDED(resource->GetSharedHandle(&entry->shared));
    if (resource) resource->Release();
    if (!shared) {
        freeGpuFrameTexture(nullptr, reinterpret_cast<uint8_t*>(entry));
        spec->failed = true;
        return nullptr;
    }
    entry->serial = ++g_gpuFrameSerial;
    entry->pool = spec->id;
    AVBufferRef* ref = av_buffer_create(reinterpret_cast<uint8_t*>(entry), size, freeGpuFrameTexture, nullptr, 0);
    if (!ref) {
        freeGpuFrameTexture(nullptr, reinterpret_cast<uint8_t*>(entry));
        return nullptr;
    }
    ++spec->created;
    return ref;
}

void freeGpuPoolSpec(void* opaque) {
    delete static_cast<GpuPoolSpec*>(opaque);
}

// A frame left on the GPU: AV_PIX_FMT_D3D11 with data[0] the pool texture, buf[0] its
// pool reference, no hw_frames_ctx (the decoder's surface pool is not held).
bool isGpuFrame(const AVFrame* frame) {
    return frame->format == AV_PIX_FMT_D3D11 && !frame->hw_frames_ctx && frame->buf[0];
}

// Copy a frame left on the GPU to memory, allocating the memory unless `copy` has it;
// false on failure.
bool downloadGpuFrame(const AVFrame* frame, AVFrame* copy, ID3D11Texture2D*& staging,
                      D3D11_TEXTURE2D_DESC& stagingDesc, const std::atomic<bool>* stop = nullptr) {
    AVD3D11VADeviceContext* d3d = sharedD3d();
    const auto* entry = reinterpret_cast<const GpuFrameTexture*>(frame->buf[0]->data);
    if (!d3d || !entry->texture) return false;
    copy->format = memoryFormatOf(entry->texture);
    copy->width = frame->width;
    copy->height = frame->height;
    if (copy->format == AV_PIX_FMT_NONE || (!copy->buf[0] && av_frame_get_buffer(copy, TRANSFER_ALIGN) < 0)) {
        return false;
    }
    if (readBackTexture(d3d, entry->texture, 0, staging, stagingDesc, copy, entry->mutex, stop) < 0) return false;
    av_frame_copy_props(copy, frame);
    return true;
}
#endif

}  // namespace

class VideoDecoder::Impl {
public:
    Impl() = default;
    ~Impl() { close(); }

    bool open(const std::filesystem::path& path, const DecodeOptions& options) {
        close();
        options_ = options;

        // FFmpeg takes UTF-8 file names on Windows; path::string() is the ANSI code page.
        const std::string pathStr = utf8FromPath(path);

        // Open input file
        const AVIOInterruptCB interrupt{&Impl::readCutRequested, this};
        if (openLocalMediaInput(&formatCtx_, pathStr.c_str(), &interrupt) < 0) {
            LOG_ERROR("Failed to open video file: {}", pathStr);
            return false;
        }

        isLive_ = false;
        return setupDecoderFromFormat();
    }

    bool openCamera(const std::string& deviceName, int width, int height, double fps) {
        close();
        options_ = DecodeOptions{};  // capture stays software, RGB24
#ifdef QUADMAPPER_HAS_AVDEVICE
        avdevice_register_all();
        const AVInputFormat* ifmt = av_find_input_format("dshow");
        if (!ifmt) {
            LOG_ERROR("dshow input format not available");
            return false;
        }

        AVDictionary* opts = nullptr;
        if (width > 0 && height > 0) {
            std::string sz = std::to_string(width) + "x" + std::to_string(height);
            av_dict_set(&opts, "video_size", sz.c_str(), 0);
        }
        if (fps > 0.0) {
            av_dict_set(&opts, "framerate", std::to_string(static_cast<int>(fps)).c_str(), 0);
        }
        // Generous real-time buffer so brief decode hitches don't drop frames
        av_dict_set(&opts, "rtbufsize", "100000000", 0);

        std::string url = "video=" + deviceName;
        int rc = avformat_open_input(&formatCtx_, url.c_str(), ifmt, &opts);
        av_dict_free(&opts);
        if (rc < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(rc, err, sizeof(err));
            LOG_ERROR("Failed to open camera '{}': {}", deviceName, err);
            return false;
        }

        isLive_ = true;
        if (!setupDecoderFromFormat()) {
            return false;
        }
        info_.duration = 0.0;   // live capture: no duration, no seek
        info_.totalFrames = 0;
        LOG_INFO("Opened camera '{}': {}x{}, {} fps", deviceName, info_.width, info_.height, info_.frameRate);
        return true;
#else
        (void)deviceName; (void)width; (void)height; (void)fps;
        LOG_ERROR("openCamera requires libavdevice (not compiled in)");
        return false;
#endif
    }

    // Shared decoder setup; assumes formatCtx_ is already open on an input.
    bool setupDecoderFromFormat() {
        // Find stream info
        if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
            LOG_ERROR("Failed to find stream info");
            close();
            return false;
        }

        // Find video stream
        videoStreamIndex_ = -1;
        audioStreamIndex_ = -1;
        for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
            if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIndex_ < 0) {
                videoStreamIndex_ = static_cast<int>(i);
            }
            if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStreamIndex_ < 0) {
                audioStreamIndex_ = static_cast<int>(i);
            }
        }

        if (videoStreamIndex_ < 0) {
            // Not a fault: audio-only files (.mp4/.mov of a sound track) are listed with the
            // videos, and the media bin asks each one for a thumbnail.
            LOG_INFO("{} has no video stream (audio only?)", formatCtx_->url ? formatCtx_->url : "The file");
            close();
            return false;
        }

        if (const AVRational base = formatCtx_->streams[videoStreamIndex_]->time_base;
            base.num <= 0 || base.den <= 0) {
            LOG_ERROR("The video stream has no valid time base");
            close();
            return false;
        }

        const AVCodecID codecId = formatCtx_->streams[videoStreamIndex_]->codecpar->codec_id;
        bool hardware = options_.hardware && !isLive_ && hardwareCodec(codecId);
        if (options_.hardware && !hardware) setDecodeNote("the GPU decoder does not handle this codec");
        if (!openCodec(hardware)) {
            close();
            return false;
        }

        // Allocate frame
        frame_ = av_frame_alloc();
        skippedFrame_ = av_frame_alloc();
        rgbFrame_ = av_frame_alloc();
        if (!frame_ || !skippedFrame_ || !rgbFrame_) {
            LOG_ERROR("Failed to allocate frames");
            close();
            return false;
        }

        if (const std::string tooLarge = pictureSizeProblem(codecCtx_->width, codecCtx_->height);
            !tooLarge.empty()) {
            LOG_ERROR("Video {}", tooLarge);
            close();
            return false;
        }

        // Setup RGB frame buffer (the scaler is made for each frame's pixel format)
        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codecCtx_->width, codecCtx_->height, 1);
        if (numBytes <= 0) {
            LOG_ERROR("Invalid video size {}x{}", codecCtx_->width, codecCtx_->height);
            close();
            return false;
        }
        rgbBuffer_.resize(numBytes);
        av_image_fill_arrays(rgbFrame_->data, rgbFrame_->linesize, rgbBuffer_.data(),
                           AV_PIX_FMT_RGB24, codecCtx_->width, codecCtx_->height, 1);

        // Fill video info
        AVStream* videoStream = formatCtx_->streams[videoStreamIndex_];
        info_.width = codecCtx_->width;
        info_.height = codecCtx_->height;
        info_.codecName = codecCtx_->codec->name;

        // Duration: the video stream's own, when the container records it, since the
        // playback clock loops at it (a longer audio track would add a frozen tail);
        // else the container's.
        if (videoStream->duration != AV_NOPTS_VALUE && videoStream->duration > 0) {
            info_.duration = static_cast<double>(videoStream->duration) * av_q2d(videoStream->time_base);
        } else if (formatCtx_->duration != AV_NOPTS_VALUE) {
            info_.duration = static_cast<double>(formatCtx_->duration) / AV_TIME_BASE;
        }

        // A header can claim any length: keep it where the frame count below fits.
        info_.duration = std::clamp(info_.duration, 0.0, MAX_DURATION_SECONDS);

        // Calculate frame rate
        AVRational frameRate = av_guess_frame_rate(formatCtx_, videoStream, nullptr);
        if (frameRate.num > 0 && frameRate.den > 0) {
            info_.frameRate = static_cast<double>(frameRate.num) / frameRate.den;
        }

        // Calculate total frames
        if (videoStream->nb_frames > 0) {
            info_.totalFrames = videoStream->nb_frames;
        } else if (info_.duration > 0 && info_.frameRate > 0) {
            info_.totalFrames = static_cast<int64_t>(info_.duration * info_.frameRate);
        }

        // Audio info
        if (audioStreamIndex_ >= 0) {
            AVCodecParameters* audioPar = formatCtx_->streams[audioStreamIndex_]->codecpar;
            info_.hasAudio = true;
            info_.audioChannels = audioPar->ch_layout.nb_channels;
            info_.audioSampleRate = audioPar->sample_rate;
        }

        timeBase_ = av_q2d(videoStream->time_base);
        // Frame times are reported relative to the stream start, so pts 0 is the
        // first frame even in containers whose timestamps begin elsewhere (MPEG-TS).
        startPts_ = videoStream->start_time != AV_NOPTS_VALUE ? videoStream->start_time : 0;
        frameTicks_ = info_.frameRate > 0.0
            ? std::max<int64_t>(1, std::llround(1.0 / (info_.frameRate * timeBase_)))
            : 1;
        frameSeconds_ = info_.frameRate > 0.0 ? 1.0 / info_.frameRate : 0.04;
        const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codecCtx_->codec_id);
        intraOnly_ = descriptor && (descriptor->props & AV_CODEC_PROP_INTRA_ONLY);
        // TD-32: AVI has no decode-order timestamp field, so its demuxer can only
        // back-fill a presentation pts for some packets of a B-frame stream (the ones
        // it can place without knowing how many more B pictures follow) and leaves the
        // rest (typically the I/P pictures) AV_NOPTS_VALUE. H.264/HEVC in AVI still
        // carry real timestamps in practice and are excluded; every other B-frame codec
        // in this container gets an unreliable, decode-order-tainted pts on the frames
        // that do have one (see AVI's own review notes). The decoder already has a
        // solid, tested fallback for a frame with no pts at all (sequential timing from
        // the last known time, ADR-050/055): forcing every frame of such a stream onto
        // that path, instead of trusting the sometimes-present pts, keeps presentation
        // order (which libavcodec's B-frame reordering already guarantees) and drops
        // the per-frame values that do not mean what they look like they mean.
        const bool aviContainer = formatCtx_->iformat && formatCtx_->iformat->name &&
            std::strstr(formatCtx_->iformat->name, "avi") != nullptr;
        unreliableFramePts_ = aviContainer && videoStream->codecpar->video_delay > 0 &&
            codecCtx_->codec_id != AV_CODEC_ID_H264 && codecCtx_->codec_id != AV_CODEC_ID_HEVC;
        const bool probeEnd = !isLive_ &&
            (videoStream->duration == AV_NOPTS_VALUE || videoStream->duration <= 0);
        // TD-33: a container can declare a duration that is present and positive but
        // still short of the real one, when an edit list trims the movie's start (the
        // "mov,mp4,m4a,3gp,3g2,mj2" demuxer reports the trimmed stream length, not the
        // frame count actually decoded past it). Cross-check that family, only widening
        // the duration, and only when the first video packet is marked for discard (the
        // sign of a start-trimming edit list): the probe reads the last second of packets,
        // which for an all-intra 4K file (ProRes) is ~100 MB on the opening thread.
        const bool movFamily = !isLive_ && formatCtx_->iformat && formatCtx_->iformat->name &&
            std::strstr(formatCtx_->iformat->name, "mov") != nullptr;
        const bool trimmedMov = movFamily && startIsTrimmedByEditList();
        if (probeEnd || trimmedMov) {
            // No video duration recorded: measure where the video really ends, since the
            // playback clock loops there (Matroska declares its end time, start offset
            // included; MPEG-TS only has an estimate).
            const double end = probeVideoEnd(info_.duration);
            if (probeEnd ? end > 0.0 : end > info_.duration + frameSeconds_ * 0.5) info_.duration = end;
        }
        seekTargetPts_ = AV_NOPTS_VALUE;
        checkFirstPacket_ = false;
        checkFirstFrame_ = false;
        keepPreroll_ = false;
        isOpen_ = true;
        currentFrame_ = 0;
        currentTime_ = 0.0;
        atEnd_ = false;
        observedEnd_ = 0.0;
        endSeen_ = false;
        // Hardware: decode the first frame now, so a stream the GPU refuses (profile,
        // size, too many sessions) is in software before playback starts.
        const bool probeHardware = hwActive_;
        if (probeHardware && !decodeNextRaw() && hwActive_ && codecCtx_) {
            resumeInSoftware("the GPU decoded no frame of this stream", startPts_);
        }
        if (!codecCtx_) {
            close();
            return false;
        }
        // Back to the first frame after reading the tail or the first frame.
        if (probeEnd || movFamily || probeHardware) seekExact(0.0);
        // Read the index now, before the decode thread exists: it owns formatCtx_ afterwards.
        widestKeyGap_ = measureWidestKeyGap();

        // The logger substitutes plain {} only (no format specs).
        LOG_INFO("Opened video: {}x{}, {} fps, {} s, {} decoding", info_.width, info_.height,
                 info_.frameRate, info_.duration, hwActive_ ? "hardware" : "software");
        return true;
    }

    // Allocate and open the codec context: on the shared D3D11 device when `hardware`
    // (falling back to software if the device or the codec cannot), else in software.
    bool openCodec(bool hardware) {
        const AVCodecParameters* codecPar = formatCtx_->streams[videoStreamIndex_]->codecpar;
        const AVCodec* codec = nullptr;
        if (hardware) {
            codec = hardwareDecoder(codecPar->codec_id);
            if (codec) hwDevice_ = acquireHardwareDevice();
            if (!codec || !hwDevice_) {
                setDecodeNote(codec ? "no GPU video decoder on this machine"
                                    : "this FFmpeg build has no D3D11 decoder for the codec");
                codec = nullptr;  // the software decoder for the codec, found below
                hardware = false;
            }
        }
        if (!codec) codec = avcodec_find_decoder(codecPar->codec_id);
        if (!codec) {
            LOG_ERROR("Unsupported codec");
            return false;
        }

        codecCtx_ = avcodec_alloc_context3(codec);
        if (!codecCtx_) {
            LOG_ERROR("Failed to allocate codec context");
            return false;
        }
        if (avcodec_parameters_to_context(codecCtx_, codecPar) < 0) {
            LOG_ERROR("Failed to copy codec parameters");
            return false;
        }

        if (hardware) {
            // The GPU decodes; one CPU thread only feeds it. Frames are copied to memory
            // as soon as they are chosen, so two extra surfaces cover the ones held here.
            codecCtx_->hw_device_ctx = av_buffer_ref(hwDevice_);
            codecCtx_->get_format = chooseHardwareFormat;
            codecCtx_->thread_count = 1;
            codecCtx_->extra_hw_frames = 2;
        } else if (!isLive_) {
            // Decode files on two cores. On one core a 20 Mbit/s 1080p H.264 clip takes
            // 35-60 ms a frame (--bench drift), longer than a frame lasts at 25 fps, so the
            // picture fell behind the clock; two threads keep it in time. Not a thread per
            // core: with many simultaneous clips that oversubscribes the CPU and starves
            // the render thread (16 such clips: 3.7 FPS on one thread, 2.6 on two, 0.8 on
            // one per core). Live capture stays single-threaded: frame threading would add
            // frames of latency to a camera.
            codecCtx_->thread_count = 2;
            codecCtx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }

        if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
            if (hardware) {
                LOG_WARNING("The GPU decoder would not open this stream; decoding in software");
                setDecodeNote("the GPU decoder would not open this stream");
                closeCodec();
                return openCodec(false);
            }
            LOG_ERROR("Failed to open codec");
            return false;
        }
        hwActive_ = hardware;
        reorderDelay_ = std::max(codecCtx_->has_b_frames, 0);
        return true;
    }

    void closeCodec() {
#ifdef _WIN32
        releaseStaging(staging_);
        stagingDesc_ = D3D11_TEXTURE2D_DESC{};
        if (copyDone_) {
            copyDone_->Release();
            g_liveD3dObjects.fetch_sub(1, std::memory_order_relaxed);
        }
        copyDone_ = nullptr;
#endif
        if (codecCtx_) avcodec_free_context(&codecCtx_);  // drops its device and surface pool
        av_buffer_unref(&hwDevice_);
        hwActive_ = false;
    }

    // The GPU refused or failed this stream: reopen it in software and decode again
    // from the keyframe before `resumePts` (stream time base), skipping to that frame,
    // so playback carries on with the same frame. Returns false if even software
    // decoding could not open (codecCtx_ is then null).
    bool resumeInSoftware(const std::string& reason, int64_t resumePts) {
        LOG_WARNING("Hardware decoding stopped ({}); this video carries on in software", reason);
        setDecodeNote(reason);
        av_frame_unref(frame_);
        av_frame_unref(skippedFrame_);
        closeCodec();
        if (!openCodec(false)) {
            closeCodec();
            return false;
        }
        const double landed = landedKeyPts_;
        if (seekDemuxer(resumePts)) {
            seekTargetPts_ = resumePts;
            keepPreroll_ = false;
            atEnd_ = false;
            // The reverse leg's group still started at its own keyframe.
            landedKeyAfterResume_ = landed;
        }
        return true;
    }

    // frame_ holds a frame to show. A hardware frame is copied to memory here (only the
    // frames shown or cached, not those skipped on the way to a seek target). False if
    // the GPU failed and decoding restarted in software: decode again.
    bool finishHardwareFrame() {
        if (!hwActive_) return true;
        const int64_t pts = frame_->pts != AV_NOPTS_VALUE
            ? frame_->pts : startPts_ + std::llround(currentTime_.load() / timeBase_);
        if (frame_->format != AV_PIX_FMT_D3D11) {
            resumeInSoftware("the GPU decoder refused this stream", pts);
            return false;
        }
        const bool injected = options_.failHardwareAfterFrames >= 0 &&
                              hwFrames_ >= options_.failHardwareAfterFrames;
        AVFrame* copy = av_frame_alloc();
        int rc = !copy ? AVERROR(ENOMEM) : injected ? AVERROR(EIO) : AVERROR(EAGAIN);
        if (rc == AVERROR(EAGAIN)) {
            // Left on the GPU where it can be (ADR-056); else, or with the pool at its
            // cap, copied to memory.
#ifdef _WIN32
            if (keepOnGpu()) rc = copyToGpuPool(frame_, copy);
#endif
            if (rc < 0) {
                av_frame_unref(copy);
                rc = transferToMemory(frame_, copy);
            }
        }
        if (rc < 0 && stopDecoding_) {
            av_frame_free(&copy);  // the thread is being joined: not a GPU failure
            return false;
        }
        if (rc < 0) {
            av_frame_free(&copy);
            resumeInSoftware(injected ? "simulated GPU failure (test)"
                                      : "copying a decoded frame from the GPU failed", pts);
            return false;
        }
        av_frame_copy_props(copy, frame_);
        av_frame_unref(frame_);
        av_frame_move_ref(frame_, copy);
        av_frame_free(&copy);
        ++hwFrames_;
        return true;
    }

    // Copy a D3D11 surface to memory (NV12, or P010 for 10-bit) in a buffer from this
    // stream's pool, so frames reuse memory instead of allocating 3 MB each.
    int transferToMemory(const AVFrame* surface, AVFrame* copy) {
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(surface->hw_frames_ctx->data);
        copy->format = frames->sw_format;
        copy->width = surface->width;
        copy->height = surface->height;
        const int filled = transferBuffer(copy);
        if (filled < 0) return filled;
#ifdef _WIN32
        const auto* device = reinterpret_cast<const AVHWDeviceContext*>(frames->device_ref->data);
        return readBackTexture(static_cast<AVD3D11VADeviceContext*>(device->hwctx),
                               reinterpret_cast<ID3D11Texture2D*>(surface->data[0]),
                               static_cast<UINT>(reinterpret_cast<intptr_t>(surface->data[1])), staging_,
                               stagingDesc_, copy, nullptr, &stopDecoding_);
#else
        return av_hwframe_transfer_data(copy, surface, 0);
#endif
    }

    // Memory for `copy` (format and size set) from the stream's pool.
    int transferBuffer(AVFrame* copy) {
        const auto format = static_cast<AVPixelFormat>(copy->format);
        const int bytes = av_image_get_buffer_size(format, copy->width, copy->height, TRANSFER_ALIGN);
        if (bytes <= 0) return AVERROR(EINVAL);
        if (!transferPool_ || transferPoolBytes_ != bytes) {
            av_buffer_pool_uninit(&transferPool_);  // buffers still in use are freed on return
            transferPool_ = av_buffer_pool_init(static_cast<size_t>(bytes), nullptr);
            transferPoolBytes_ = bytes;
        }
        copy->buf[0] = transferPool_ ? av_buffer_pool_get(transferPool_) : nullptr;
        if (!copy->buf[0]) return AVERROR(ENOMEM);
        return av_image_fill_arrays(copy->data, copy->linesize, copy->buf[0]->data, format, copy->width,
                                    copy->height, TRANSFER_ALIGN);
    }

    // Whether frames are left on the GPU for the zero-copy path (ADR-056).
    bool keepOnGpu() const {
#ifdef _WIN32
        return options_.zeroCopy && options_.yuvOutput && !zeroCopyStopped_;
#else
        return false;
#endif
    }

#ifdef _WIN32
    // Copy a D3D11 decoder surface into a texture of this stream's own pool, so the
    // frame queue never holds the decoder's few surfaces. EAGAIN when the pool is at its
    // cap, or when zero-copy has just stopped because the device cannot share the format
    // or make the texture: the caller copies the frame to memory instead.
    int copyToGpuPool(const AVFrame* surface, AVFrame* copy) {
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(surface->hw_frames_ctx->data);
        const auto* device = reinterpret_cast<const AVHWDeviceContext*>(frames->device_ref->data);
        auto* d3d = static_cast<AVD3D11VADeviceContext*>(device->hwctx);
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(surface->data[0]);
        const auto index = static_cast<UINT>(reinterpret_cast<intptr_t>(surface->data[1]));

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!gpuPool_ || gpuPoolDesc_.Width != desc.Width || gpuPoolDesc_.Height != desc.Height ||
            gpuPoolDesc_.Format != desc.Format) {
            releaseGpuPool();
            // Without shared NV12/P010 textures frames would silently go through memory
            // while the stream claimed zero-copy.
            if (options_.denySharedFormat || !canShareFormat(d3d->device, desc.Format)) {
                stopZeroCopy(std::string("the GPU cannot share ") +
                             (desc.Format == DXGI_FORMAT_P010 ? "P010" : "NV12") + " textures" +
                             (options_.denySharedFormat ? " (test)" : ""));
                return AVERROR(EAGAIN);
            }
            auto* spec = new GpuPoolSpec;
            spec->device = d3d->device;
            spec->id = ++g_gpuPoolSerial;
            spec->failAfter = options_.failGpuTextureAfter;
            spec->desc.Width = desc.Width;
            spec->desc.Height = desc.Height;
            spec->desc.MipLevels = 1;
            spec->desc.ArraySize = 1;
            spec->desc.Format = desc.Format;
            spec->desc.SampleDesc.Count = 1;
            spec->desc.Usage = D3D11_USAGE_DEFAULT;
            spec->desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // the zero-copy pass reads its planes
            // Opened by the renderer's own D3D11 device, so its thread never waits on the
            // decoders' device (ADR-056 amendment).
            spec->desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            // The frame's size in memory, so the reverse cache budgets GPU frames alike.
            const int bytes = av_image_get_buffer_size(frames->sw_format, surface->width, surface->height, 1);
            gpuPool_ = av_buffer_pool_init2(static_cast<size_t>(std::max(bytes, 1)), spec,
                                            allocGpuFrameTexture, freeGpuPoolSpec);
            if (!gpuPool_) {
                delete spec;
                return AVERROR(ENOMEM);
            }
            gpuPoolSpec_ = spec;
            gpuPoolDesc_ = desc;
        }
        copy->buf[0] = av_buffer_pool_get(gpuPool_);
        if (!copy->buf[0]) {
            // Below the cap a texture could not be made (video memory full): stop once,
            // rather than try again every frame while claiming zero-copy.
            if (gpuPoolSpec_->failed) stopZeroCopy("out of video memory for this stream's GPU frames");
            return AVERROR(EAGAIN);
        }
        auto* entry = reinterpret_cast<GpuFrameTexture*>(copy->buf[0]->data);

        // A pool texture is only handed out once no frame holds it, so the renderer never
        // holds its mutex here and the wait is at most a moment.
        if (!copyDone_) {
            const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
            if (FAILED(d3d->device->CreateQuery(&query, &copyDone_))) {
                copyDone_ = nullptr;
                return AVERROR(ENOMEM);
            }
            g_liveD3dObjects.fetch_add(1, std::memory_order_relaxed);
        }
        d3d->lock(d3d->lock_ctx);
        if (entry->mutex->AcquireSync(0, KEYED_MUTEX_WAIT_MS) != S_OK) {
            d3d->unlock(d3d->lock_ctx);
            return AVERROR_EXTERNAL;
        }
        d3d->device_context->CopySubresourceRegion(entry->texture, 0, 0, 0, 0, texture, index, nullptr);
        entry->mutex->ReleaseSync(0);
        d3d->device_context->End(copyDone_);
        d3d->device_context->Flush();
        d3d->unlock(d3d->lock_ctx);

        // Queue the frame only once the copy is done on the GPU, as the readback path
        // waits for its copy: the copy sits in the decoders' one queue behind every
        // stream's decoding, and a renderer that met it unfinished would wait on all of
        // that (after a loop, the app ran at ~24 fps for a second; ADR-056 amendment).
        const bool done = waitForGpu([&] {
            d3d->lock(d3d->lock_ctx);
            // Flushing allowed: with DONOTFLUSH the event never completed after the keyed
            // mutex release (measured: every poll ran out, 2 s a frame).
            const HRESULT hr = d3d->device_context->GetData(copyDone_, nullptr, 0, 0);
            d3d->unlock(d3d->lock_ctx);
            return hr == S_OK ? 1 : FAILED(hr) ? -1 : 0;
        }, &stopDecoding_);
        if (!done) return stopDecoding_ ? AVERROR_EXIT : AVERROR_EXTERNAL;

        copy->format = AV_PIX_FMT_D3D11;
        copy->width = surface->width;
        copy->height = surface->height;
        copy->data[0] = reinterpret_cast<uint8_t*>(entry->texture);
        copy->data[1] = nullptr;  // array slice 0
        return 0;
    }

    void releaseGpuPool() {
        av_buffer_pool_uninit(&gpuPool_);  // textures still in frames are freed on release
        gpuPoolSpec_ = nullptr;            // freed with the pool's last texture
        gpuPoolDesc_ = D3D11_TEXTURE2D_DESC{};
    }

    // Zero-copy stopped (ADR-056), on the decode thread, once: the pool is released (its
    // textures go as the frames holding them do), and the frames queued on the GPU are
    // copied to memory here, not on the main thread, which keeps showing its last frame
    // meanwhile. The queue is taken out while they are copied; only this thread adds to it.
    // A reposition handed over meanwhile (startPlayback) empties the queue: the frames
    // taken out belong to the plan it replaced and are dropped, not put back in front of
    // the new plan's (they would hold its frames back and freeze the clip on a wrong one).
    void leaveGpu() {
        if (!zeroCopyStopped_ || leftGpu_) return;
        leftGpu_ = true;
        releaseGpuPool();
        std::queue<VideoFrame> queued;
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            std::swap(queued, frameBuffer_);
        }
        std::queue<VideoFrame> inMemory;
        for (; !queued.empty() && !stopDecoding_; queued.pop()) {
            VideoFrame& frame = queued.front();
            if (!frame.yuv.onGpu()) {
                inMemory.push(std::move(frame));
                continue;
            }
            VideoFrame copied = frameInMemory(frame);
            if (copied.isValid()) inMemory.push(std::move(copied));
        }
        if (options_.testDuringLeaveGpu) options_.testDuringLeaveGpu();
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            if (replan_) inMemory = std::queue<VideoFrame>();
            for (; !frameBuffer_.empty(); frameBuffer_.pop()) inMemory.push(std::move(frameBuffer_.front()));
            std::swap(frameBuffer_, inMemory);
        }
        bufferCondition_.notify_all();
    }

    // A queued frame left on the GPU, copied to memory from the stream's pool (freeing
    // pooled memory costs the main thread nothing when the frames it drops are released);
    // invalid on failure.
    VideoFrame frameInMemory(const VideoFrame& gpuFrame) {
        VideoFrame result;
        result.width = gpuFrame.width;
        result.height = gpuFrame.height;
        result.pts = gpuFrame.pts;
        result.due = gpuFrame.due;
        result.frameNumber = gpuFrame.frameNumber;
        const auto* frame = static_cast<const AVFrame*>(gpuFrame.yuv.owner.get());  // see shareGpuFrame()
        AVFrame* downloaded = av_frame_alloc();
        if (downloaded && frame && isGpuFrame(frame)) {
            downloaded->format = memoryFormatOf(reinterpret_cast<const GpuFrameTexture*>(frame->buf[0]->data)->texture);
            downloaded->width = frame->width;
            downloaded->height = frame->height;
        }
        if (downloaded && frame && downloaded->format != AV_PIX_FMT_NONE && transferBuffer(downloaded) >= 0 &&
            downloadGpuFrame(frame, downloaded, staging_, stagingDesc_, &stopDecoding_)) {
            if (!shareYuv(downloaded, result)) convertToRgb(downloaded, result);
        } else {
            if (!stopDecoding_) LOG_WARNING("Copying a GPU frame to memory failed; frame {} is skipped", gpuFrame.frameNumber);
            result = VideoFrame{};
        }
        av_frame_free(&downloaded);
        return result;
    }
#endif

    void setDecodeNote(const std::string& note) {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        decodeNote_ = note;
    }

    void close() {
        stopBackgroundDecoding();
        releaseReverse();
        releaseTail();

        if (swsCtx_) {
            sws_freeContext(swsCtx_);
            swsCtx_ = nullptr;
        }
        if (frame_) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (skippedFrame_) {
            av_frame_free(&skippedFrame_);
            skippedFrame_ = nullptr;
        }
        if (rgbFrame_) {
            av_frame_free(&rgbFrame_);
            rgbFrame_ = nullptr;
        }
        closeCodec();
        av_buffer_pool_uninit(&transferPool_);  // buffers still in frames are freed on release
        transferPoolBytes_ = 0;
#ifdef _WIN32
        releaseGpuPool();
        leftGpu_ = false;
#endif
        zeroCopyStopped_ = false;
        if (formatCtx_) {
            avformat_close_input(&formatCtx_);
            formatCtx_ = nullptr;
        }

        rgbBuffer_.clear();
        info_ = VideoInfo{};
        videoStreamIndex_ = -1;
        audioStreamIndex_ = -1;
        isOpen_ = false;
        isLive_ = false;
        planned_ = false;
        currentFrame_ = 0;
        currentTime_ = 0.0;
        atEnd_ = false;
        observedEnd_ = 0.0;
        endSeen_ = false;
        reversePeakBytes_ = 0;
        hwFrames_ = 0;
        landedKeyAfterResume_.reset();
        rgbColorKey_ = -1;
        setDecodeNote({});
        std::lock_guard<std::mutex> lock(bufferMutex_);
        zeroCopyNote_.clear();
    }

    bool isOpen() const { return isOpen_; }
    bool isHardwareDecoding() const { return hwActive_; }
    std::string getDecodeNote() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return decodeNote_;
    }
    bool isZeroCopy() const { return hwActive_ && keepOnGpu(); }
    void stopZeroCopy(const std::string& reason) {
        if (zeroCopyStopped_.exchange(true)) return;
        LOG_WARNING("Zero-copy video stopped ({}); this video's frames are copied through memory", reason);
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            zeroCopyNote_ = reason;
        }
        bufferCondition_.notify_all();  // a decode thread waiting on a full queue: leaveGpu()
    }
    std::string getZeroCopyNote() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return zeroCopyNote_;
    }
    bool isLive() const { return isLive_; }
    const VideoInfo& getInfo() const { return info_; }
    double getCurrentTime() const { return currentTime_; }
    int64_t getCurrentFrame() const { return currentFrame_; }
    bool isAtEnd() const { return atEnd_; }
    double getObservedEnd() const { return endSeen_ ? observedEnd_.load() : 0.0; }
    size_t getReverseCachePeakBytes() const { return reversePeakBytes_; }
    uint64_t decoderRestarts() const { return restarts_; }

    bool seek(double timeSeconds) {
        if (!isOpen_) return false;
        stopBackgroundDecoding();  // seekExact moves the demuxer and codec the decode thread uses
        if (!seekExact(timeSeconds)) return false;
        planned_ = false;  // back to plain forward decoding

        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            std::queue<VideoFrame> empty;
            std::swap(frameBuffer_, empty);
        }
        bufferCondition_.notify_all();

        return true;
    }

    VideoFrame decodeNextFrame() {
        if (!isOpen_ || !decodeNextRaw()) return VideoFrame{};
        return convertFrame(frame_);
    }

    // --- Planned playback (ADR-050) ---

    // Hands the plan to the decode thread (started if need be), which positions itself
    // there; the caller never waits for it (TD-35: joining the thread to reposition
    // blocked the render thread up to 1.7 s). Frames of the previous plan are dropped
    // here and any still being decoded are never queued: the replan flag is raised under
    // the lock the decode thread queues frames under.
    void startPlayback(const DecodePlan& plan, size_t bufferSize) {
        if (!isOpen_) return;
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            pendingPlan_ = plan;
            replan_ = true;
            atEnd_ = false;  // the new plan starts somewhere to decode
            maxBufferSize_ = bufferSize;
            std::queue<VideoFrame> empty;
            std::swap(frameBuffer_, empty);
            lastError_.clear();
            reverseError_.clear();
        }
        bufferCondition_.notify_all();
        startBackgroundDecoding(bufferSize);
    }

    // Decode thread: position the decoder at the latest plan startPlayback() handed over.
    void applyPendingPlan() {
        DecodePlan plan;
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            plan = pendingPlan_;
            replan_ = false;
        }
        releaseReverse();
        plan_ = plan;
        planned_ = true;
        planStalled_ = false;
        emptyLegs_ = 0;
        releaseTail();
        tailOverflow_ = false;

        leg_ = legAt(plan.timeline, plan.duration, plan.loopMode);
        const ClipPosition clip = mapTimeline(plan.timeline, plan.duration, plan.loopMode);
        if (leg_.direction > 0) {
            beginForward(clip.position);
        } else {
            // Mid reverse leg: first show the frame holding the position, then earlier ones.
            beginReverse(clip.position + FRAME_DUE_EPSILON);
        }
    }

    // The widest gap between keyframes in the demuxer's index, or none when it has no keyframes
    // yet. Walks the AVFormatContext, so it runs on the opening thread only.
    std::optional<double> measureWidestKeyGap() const {
        if (isLive_ || intraOnly_) return std::nullopt;
        AVStream* stream = formatCtx_->streams[videoStreamIndex_];
        const int entries = avformat_index_get_entries_count(stream);
        double previousKey = std::numeric_limits<double>::quiet_NaN();
        double widestGap = 0.0;
        for (int i = 0; i < entries; ++i) {
            const AVIndexEntry* entry = avformat_index_get_entry(stream, i);
            if (!entry || !(entry->flags & AVINDEX_KEYFRAME)) continue;
            const double t = static_cast<double>(entry->timestamp - startPts_) * timeBase_;
            if (!std::isnan(previousKey)) widestGap = std::max(widestGap, t - previousKey);
            previousKey = t;
        }
        if (std::isnan(previousKey)) return std::nullopt;  // no index: checked while playing
        return std::max(widestGap, info_.duration - previousKey);  // the last group
    }

    std::string checkReverse(size_t cacheBytes) const {
        if (!isOpen_ || isLive_) return "the source cannot seek.";
        if (intraOnly_) return {};  // every frame is a keyframe
        if (!widestKeyGap_) return {};  // no index: checked while playing
        const double widestGap = *widestKeyGap_;

        // One group of pictures, plus the reordering delay's frames decoded past it.
        const int64_t frames = static_cast<int64_t>(std::ceil(widestGap / frameSeconds_ - 1e-6)) +
                               reorderDelay_.load() + 1;
        // The cache holds frames in memory in the stream's own format (hardware frames
        // are copied out as NV12/P010, the same size as 4:2:0 YUV): 1.5 bytes a pixel
        // for 8-bit 4:2:0, where RGB24 would take 3.
        const auto format = static_cast<AVPixelFormat>(
            formatCtx_->streams[videoStreamIndex_]->codecpar->format);
        const int perFrame = av_image_get_buffer_size(format, info_.width, info_.height, 1);
        if (perFrame <= 0) return {};
        const double needed = static_cast<double>(frames) * perFrame;
        if (needed <= static_cast<double>(cacheBytes)) return {};
        return "its keyframes are " + std::to_string(frames - 1) + " frames apart; at " +
               std::to_string(info_.width) + "x" + std::to_string(info_.height) +
               " one group of pictures needs " + formatBytes(needed) + ", " + reverseAdvice(cacheBytes);
    }

    std::string getReverseError() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return reverseError_;
    }

    // Background decoding
    void startBackgroundDecoding(size_t bufferSize) {
        if (backgroundThread_.joinable()) {
            {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                if (!threadExited_) return;
            }
            backgroundThread_.join();  // it stopped on an exception: start again
        }

        maxBufferSize_ = bufferSize;
        stopDecoding_ = false;
        threadExited_ = false;
        backgroundThread_ = std::thread(&Impl::backgroundDecodingLoop, this);
    }

    void startIdleThread() {
        if (!isOpen_ || backgroundThread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            awaitPlan_ = true;
        }
        startBackgroundDecoding(maxBufferSize_);
    }

    void stopBackgroundDecoding() {
        if (!backgroundThread_.joinable()) return;

        // Set the flag under the mutex the decode thread waits on: set outside it,
        // the notify can land between the thread's predicate check and its wait,
        // and join() then hangs on a thread that never wakes.
        {
            std::unique_lock<std::mutex> lock(bufferMutex_);
            stopDecoding_ = true;
            bufferCondition_.notify_all();
            // The thread ends within a few milliseconds unless it is reading a file
            // that has stopped answering (media on a network share that went away).
            // Only then are its reads cut short, so that this thread is not held for
            // every one of them. A read already inside Windows still has to return.
            cutReads_ = !bufferCondition_.wait_for(lock, STOP_GRACE, [this] { return threadExited_; });
        }
        backgroundThread_.join();
        if (cutReads_.exchange(false) && formatCtx_ && formatCtx_->pb) {
            // A cut read leaves the file marked "error, at its end", and the demuxer
            // possibly mid-packet. The seek that follows every stop of a file's thread
            // repositions the demuxer; it must find the file readable.
            formatCtx_->pb->error = 0;
            formatCtx_->pb->eof_reached = 0;
            lastReadError_ = 0;  // the cut is not the reason for a later failure
        }

        // Clear buffer
        std::lock_guard<std::mutex> lock(bufferMutex_);
        std::queue<VideoFrame> empty;
        std::swap(frameBuffer_, empty);
        lastError_.clear();
        replan_ = false;  // a plan the thread never took is dropped with it
    }

    VideoFrame getBufferedFrame() {
        std::unique_lock<std::mutex> lock(bufferMutex_);
        if (frameBuffer_.empty()) {
            return VideoFrame{};
        }

        VideoFrame frame = std::move(frameBuffer_.front());
        frameBuffer_.pop();
        bufferCondition_.notify_one();
        return frame;
    }

    bool peekBufferedDue(double& due) const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (frameBuffer_.empty()) return false;
        due = frameBuffer_.front().due;
        return true;
    }

    YuvPicture peekBufferedPicture() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return frameBuffer_.empty() ? YuvPicture{} : frameBuffer_.front().yuv;
    }

    bool hasBufferedFrames() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return !frameBuffer_.empty();
    }

    size_t getBufferedFrameCount() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return frameBuffer_.size();
    }

    std::string getLastError() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return lastError_;
    }

private:
    // One group of pictures decoded for the reverse leg, oldest frame first.
    struct ReverseChunk {
        std::vector<AVFrame*> frames;
        size_t bytes = 0;
        double bound = 0.0;    // holds the frames with pts < bound
        double target = 0.0;   // clip time the demuxer was sent to (keyframe at or before)
        double stopPts = std::numeric_limits<double>::quiet_NaN();  // first frame at the bound
        bool started = false;
        bool complete = false;
        bool hitEnd = false;   // decoding reached the end of the stream
        bool legEnd = false;   // nothing precedes the bound: the reverse leg is done
        int attempts = 0;
    };

    enum class Phase { Forward, Reverse, Done };

    // Decode until frame_ holds the next frame to show. False at the end of the stream
    // (atEnd_), on a read error, or when asked to stop.
    bool decodeNextRaw() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) return false;

        bool got = false;
        for (;;) {
            // Skipping to a seek target can decode seconds of video, and a scrub hands
            // over a new plan each frame: give up promptly when asked to stop or start
            // over; the next seek() resets the skip and a plain resume carries on from
            // where it stopped.
            if (interrupted()) break;

            int receiveResult = avcodec_receive_frame(codecCtx_, frame_);
            if (receiveResult == AVERROR_EOF && checkFirstFrame_ && retrySeekEarlier()) {
                continue;  // the seek landed after the last keyframe: nothing decoded
            }
            if (receiveResult == AVERROR_EOF && seekTargetPts_ != AV_NOPTS_VALUE &&
                skippedFrame_->buf[0]) {
                // The seek target lies past the last frame, so that frame is the one to show.
                av_frame_move_ref(frame_, skippedFrame_);
                seekTargetPts_ = AV_NOPTS_VALUE;
                receiveResult = 0;
            }
            if (receiveResult == AVERROR_EOF) {
                // Fully drained: every frame of the stream has been returned.
                atEnd_ = true;
                break;
            }
            if (receiveResult >= 0) {
                // TD-32: this stream's pts, where the demuxer supplied one at all, is
                // decode-order-tainted (see open()); treat every frame as if it had none.
                if (unreliableFramePts_) frame_->pts = AV_NOPTS_VALUE;
                if (!reachedSeekTarget()) continue;
                if (!finishHardwareFrame()) {
                    if (!codecCtx_) break;  // not even software decoding could open
                    continue;               // restarted in software: decode this frame again
                }
                if (landedKeyAfterResume_) {
                    landedKeyPts_ = *landedKeyAfterResume_;
                    landedKeyAfterResume_.reset();
                }
                reorderDelay_ = std::max(codecCtx_->has_b_frames, 0);
                got = true;
                break;
            }

            // The decoder wants more input (EAGAIN; a decode error also moves on).
            int readResult = av_read_frame(formatCtx_, packet);
            if (readResult == AVERROR_EOF) {
                if (spliceNextPass(packet)) continue;
                // Enter draining mode so the frames still held for B-frame reordering
                // come out. Already draining (only after a decode error) means done.
                if (avcodec_send_packet(codecCtx_, nullptr) < 0) {
                    atEnd_ = true;
                    break;
                }
                continue;
            }
            if (readResult < 0) {
                lastReadError_ = readResult;
                break;
            }

            if (packet->stream_index == videoStreamIndex_) {
                // A seek that lands on a non-key packet resumes at the next keyframe.
                // TD-14b test hook: pretend this packet landed off-key regardless of its
                // real flags, standing in for a demuxer whose index is that coarse.
                const bool forcedOffKey = checkFirstPacket_ && testOffKeyRemaining_ > 0;
                if (forcedOffKey) --testOffKeyRemaining_;
                const bool landedOffKey =
                    checkFirstPacket_ && (forcedOffKey || !(packet->flags & AV_PKT_FLAG_KEY));
                if (checkFirstPacket_ && !landedOffKey) {
                    landedKeyPts_ = packet->pts != AV_NOPTS_VALUE
                        ? static_cast<double>(packet->pts - startPts_) * timeBase_
                        : -std::numeric_limits<double>::infinity();
                }
                checkFirstPacket_ = false;
                if (!landedOffKey || !retrySeekEarlier()) {
                    avcodec_send_packet(codecCtx_, packet);  // a corrupt packet is skipped
                }
            }
            av_packet_unref(packet);
        }

        av_packet_free(&packet);
        return got;
    }

    double framePts(const AVFrame* frame) const {
        return frame->pts != AV_NOPTS_VALUE
            ? static_cast<double>(frame->pts - startPts_) * timeBase_
            : currentTime_.load();
    }

    VideoFrame convertFrame(const AVFrame* frame) {
        VideoFrame result;
        result.width = info_.width;
        result.height = info_.height;
        result.frameNumber = currentFrame_++;

        if (frame->pts != AV_NOPTS_VALUE) {
            result.pts = framePts(frame);
            currentTime_ = result.pts;
        } else {
            result.pts = currentTime_;
            // std::atomic<double> has no operator+= in C++17; single writer
            // (this thread), so load+store is correct.
            currentTime_.store(currentTime_.load() + frameSeconds_);
        }
        result.due = result.pts;

#ifdef _WIN32
        if (isGpuFrame(frame) && !keepOnGpu()) {
            // Zero-copy stopped (ADR-056): a frame cached on the GPU before that (Ping-Pong's
            // tail or reverse group) comes to memory now.
            AVFrame* downloaded = av_frame_alloc();
            const bool ok = downloaded && downloadGpuFrame(frame, downloaded, staging_, stagingDesc_, &stopDecoding_);
            if (ok) {
                if (!shareYuv(downloaded, result)) convertToRgb(downloaded, result);
            } else {
                LOG_WARNING("Copying a GPU frame to memory failed; frame {} is skipped", result.frameNumber);
            }
            av_frame_free(&downloaded);
            return result;
        }
#endif
        if (options_.yuvOutput && shareYuv(frame, result)) return result;
        convertToRgb(frame, result);
        return result;
    }

    // Hand the frame's planes on as they are (a new reference, no pixel copy) when the
    // GPU pass handles its layout.
    bool shareYuv(const AVFrame* frame, VideoFrame& result) const {
        YuvPicture& yuv = result.yuv;
        bool fullRange = frame->color_range == AVCOL_RANGE_JPEG ||
                         (frame->color_range == AVCOL_RANGE_UNSPECIFIED &&
                          codecCtx_->color_range == AVCOL_RANGE_JPEG);
#ifdef _WIN32
        if (isGpuFrame(frame)) return shareGpuFrame(frame, fullRange, result);
#endif
        switch (frame->format) {
            case AV_PIX_FMT_NV12:        yuv.layout = YuvLayout::NV12; break;
            case AV_PIX_FMT_P010LE:      yuv.layout = YuvLayout::P010; break;
            case AV_PIX_FMT_YUV420P:     yuv.layout = YuvLayout::YUV420P; break;
            case AV_PIX_FMT_YUVJ420P:    yuv.layout = YuvLayout::YUV420P; fullRange = true; break;
            case AV_PIX_FMT_YUV420P10LE: yuv.layout = YuvLayout::YUV420P10; break;
            default: return false;
        }
        for (int p = 0; p < yuv.planeCount(); ++p) {
            if (!frame->data[p] || frame->linesize[p] <= 0) {
                yuv.layout = YuvLayout::None;
                return false;
            }
        }
        AVFrame* handle = av_frame_clone(frame);
        if (!handle) {
            yuv.layout = YuvLayout::None;
            return false;
        }
        yuv.width = frame->width;
        yuv.height = frame->height;
        for (int p = 0; p < yuv.planeCount(); ++p) {
            yuv.planes[p] = handle->data[p];
            yuv.strides[p] = handle->linesize[p];
        }
        yuv.matrix = matrixOf(frame);
        yuv.fullRange = fullRange;
        yuv.owner = std::shared_ptr<const void>(handle, freeFrameHandle);
        result.width = yuv.width;
        result.height = yuv.height;
        return true;
    }

#ifdef _WIN32
    // Hand a frame left on the GPU on by its texture (ADR-056). The picture's owner is an
    // AVFrame reference to the pool texture, which downloadPicture() relies on.
    bool shareGpuFrame(const AVFrame* frame, bool fullRange, VideoFrame& result) const {
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        AVD3D11VADeviceContext* d3d = sharedD3d();
        const AVPixelFormat format = memoryFormatOf(texture);
        if (!d3d || format == AV_PIX_FMT_NONE) return false;
        AVFrame* handle = av_frame_clone(frame);
        if (!handle) return false;
        YuvPicture& yuv = result.yuv;
        yuv.layout = format == AV_PIX_FMT_P010LE ? YuvLayout::P010 : YuvLayout::NV12;
        yuv.width = frame->width;
        yuv.height = frame->height;
        yuv.matrix = matrixOf(frame);
        yuv.fullRange = fullRange;
        yuv.owner = std::shared_ptr<const void>(handle, freeFrameHandle);
        const auto* entry = reinterpret_cast<const GpuFrameTexture*>(frame->buf[0]->data);
        yuv.gpu.texture = texture;
        yuv.gpu.shared = entry->shared;
        yuv.gpu.serial = entry->serial;
        yuv.gpu.pool = entry->pool;
        yuv.gpu.device = d3d->device;
        result.width = yuv.width;
        result.height = yuv.height;
        return true;
    }
#endif

    // The matrix the frame was encoded with; untagged video follows the usual rule
    // (HD and larger BT.709, SD BT.601).
    YuvMatrix matrixOf(const AVFrame* frame) const {
        AVColorSpace space = frame->colorspace;
        if (space == AVCOL_SPC_UNSPECIFIED) space = codecCtx_->colorspace;
        switch (space) {
            case AVCOL_SPC_BT709:
            case AVCOL_SPC_SMPTE240M:  // near enough to BT.709
                return YuvMatrix::BT709;
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
            case AVCOL_SPC_FCC:
                return YuvMatrix::BT601;
            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL:
                return YuvMatrix::BT2020;
            default: {
                // JPEG-derived pictures (MJPEG) are BT.601 whatever their size.
                const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
                if (desc && std::string(desc->name).rfind("yuvj", 0) == 0) return YuvMatrix::BT601;
                return frame->height > 576 ? YuvMatrix::BT709 : YuvMatrix::BT601;
            }
        }
    }

    // RGB24 on the CPU with swscale: for RGB24 output, and pixel formats the GPU pass
    // does not handle. For playback (yuvOutput) it uses the frame's matrix and range,
    // so those formats match the GPU pass; one-off decodes (thumbnails, cameras) keep
    // swscale's defaults.
    void convertToRgb(const AVFrame* frame, VideoFrame& result) {
        const auto format = static_cast<AVPixelFormat>(frame->format);
        SwsContext* previous = swsCtx_;
        swsCtx_ = sws_getCachedContext(swsCtx_, frame->width, frame->height, format, info_.width,
                                       info_.height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr,
                                       nullptr, nullptr);
        if (!swsCtx_) {
            LOG_ERROR("No converter from pixel format {} to RGB", av_get_pix_fmt_name(format));
            return;  // an invalid frame: counted as a decode failure
        }
        if (options_.yuvOutput) {
            const YuvMatrix matrix = matrixOf(frame);
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
            const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG ||
                                   (desc && std::string(desc->name).rfind("yuvj", 0) == 0);
            const int colorKey = static_cast<int>(matrix) * 2 + (fullRange ? 1 : 0);
            if (swsCtx_ != previous || rgbColorKey_ != colorKey) {
                const int space = matrix == YuvMatrix::BT709   ? SWS_CS_ITU709
                                : matrix == YuvMatrix::BT2020 ? SWS_CS_BT2020
                                                              : SWS_CS_ITU601;
                // swscale ignores this for RGB sources, which have no matrix.
                sws_setColorspaceDetails(swsCtx_, sws_getCoefficients(space), fullRange ? 1 : 0,
                                         sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
                rgbColorKey_ = colorKey;
            }
        }
        // On failure rgbFrame_ still holds the previous frame; leave result empty instead.
        if (sws_scale(swsCtx_, frame->data, frame->linesize, 0, frame->height, rgbFrame_->data,
                      rgbFrame_->linesize) <= 0) {
            LOG_ERROR("Converting a {} frame to RGB failed", av_get_pix_fmt_name(format));
            return;  // an invalid frame: counted as a decode failure
        }

        const int rowBytes = info_.width * 3;
        result.data.resize(static_cast<size_t>(rowBytes) * info_.height);
        for (int y = 0; y < info_.height; y++) {
            memcpy(result.data.data() + static_cast<size_t>(y) * rowBytes,
                   rgbFrame_->data[0] + y * rgbFrame_->linesize[0],
                   rowBytes);
        }
    }

    // Is the first video packet marked for discard, as a start-trimming edit list marks
    // its pre-roll (TD-33)? Reads up to the first video packet; the caller seeks back.
    bool startIsTrimmedByEditList() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) return false;
        bool trimmed = false;
        for (int i = 0; i < 1000 && av_read_frame(formatCtx_, packet) >= 0; ++i) {
            const bool video = packet->stream_index == videoStreamIndex_;
            if (video) trimmed = (packet->flags & AV_PKT_FLAG_DISCARD) != 0;
            av_packet_unref(packet);
            if (video) break;
        }
        av_packet_free(&packet);
        return trimmed;
    }

    // End of the last video packet (pts + duration, relative to the stream start),
    // read from the packets after the keyframe before `declared` seconds; 0 if unknown.
    // Reads packets only (no decoding); the caller seeks back to the start.
    double probeVideoEnd(double declared) {
        const double target = std::max(0.0, declared - 1.0);
        if (av_seek_frame(formatCtx_, videoStreamIndex_, startPts_ + std::llround(target / timeBase_),
                          AVSEEK_FLAG_BACKWARD) < 0) {
            return 0.0;
        }
        AVPacket* packet = av_packet_alloc();
        if (!packet) return 0.0;
        double end = 0.0;
        for (int i = 0; i < 100000 && av_read_frame(formatCtx_, packet) >= 0; ++i) {
            if (packet->stream_index == videoStreamIndex_ && packet->pts != AV_NOPTS_VALUE) {
                const double length = packet->duration > 0 ? packet->duration * timeBase_ : frameSeconds_;
                end = std::max(end, static_cast<double>(packet->pts - startPts_) * timeBase_ + length);
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        return end;
    }

    // Frame-exact seek (QM-091): the next decoded frame is the one on screen at t.
    bool seekExact(double timeSeconds) {
        const int64_t target = startPts_ + std::llround(timeSeconds / timeBase_);
        if (!seekDemuxer(target)) {
            LOG_WARNING("Seek failed");
            return false;
        }
        seekTargetPts_ = target;
        keepPreroll_ = false;
        atEnd_ = false;
        currentTime_ = timeSeconds;
        currentFrame_ = static_cast<int64_t>(timeSeconds * info_.frameRate);
        // TD-14b test hooks: simulate this many misses of the real seek-retry mechanism
        // before it is allowed to see the truth and land normally.
        testLateRemaining_ = options_.testForceSeekLandsLateFrames;
        testOffKeyRemaining_ = options_.testForceSeekLandsOffKeyFrames;
        return true;
    }

    // Position at the keyframe at or before t and decode every frame from there.
    bool seekKeyframe(double timeSeconds) {
        const int64_t target = startPts_ + std::llround(timeSeconds / timeBase_);
        if (!seekDemuxer(target)) return false;
        seekTargetPts_ = target;
        keepPreroll_ = true;
        atEnd_ = false;
        return true;
    }

    // Position the demuxer at a keyframe at or before `pts` (stream time base) and
    // reset the decoder, which also clears its drained state after an EOF.
    bool seekDemuxer(int64_t pts) {
        if (av_seek_frame(formatCtx_, videoStreamIndex_, pts, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(codecCtx_);
        ++restarts_;
        av_frame_unref(skippedFrame_);
        seekFromPts_ = pts;
        checkFirstPacket_ = true;
        checkFirstFrame_ = true;
        landedKeyPts_ = -std::numeric_limits<double>::infinity();
        return true;
    }

    // Demuxer seeks are not always keyframe-exact: MPEG-TS bisects on packet
    // timestamps and can stop on a non-key packet, so decoding would resume at the
    // next keyframe, past the target (or reach EOF with no frame when none follows).
    // Seek back further, doubling the distance each time; false once the demuxer was
    // already positioned before the stream start, where nothing earlier exists.
    bool retrySeekEarlier() {
        checkFirstPacket_ = false;
        checkFirstFrame_ = false;
        if (seekFromPts_ < startPts_) return false;
        const int64_t oneSecond = std::llround(1.0 / timeBase_);
        const int64_t back = std::max(2 * (seekTargetPts_ - seekFromPts_), oneSecond);
        if (seekDemuxer(seekTargetPts_ - back)) return true;
        LOG_WARNING("Seek landed after the target and could not back off");
        return false;
    }

    // After a seek, whether frame_ is the one to show: a first frame past the target
    // means the seek landed late and is retried; frames that end before the target
    // are skipped, so the seek is frame-exact. A keyframe seek keeps them.
    bool reachedSeekTarget() {
        if (seekTargetPts_ == AV_NOPTS_VALUE || frame_->pts == AV_NOPTS_VALUE) return true;

        if (checkFirstFrame_) {
            checkFirstFrame_ = false;
            // TD-14b test hook: pretend this frame landed past the target regardless of
            // its real pts, standing in for a demuxer whose own seek missed.
            const bool forcedLate = testLateRemaining_ > 0;
            if (forcedLate) --testLateRemaining_;
            if ((forcedLate || frame_->pts > seekTargetPts_) && retrySeekEarlier()) return false;
        }
        if (keepPreroll_) return true;

        if (frame_->pts + frameTicks_ <= seekTargetPts_) {
            // Held in case the stream ends before the target.
            av_frame_unref(skippedFrame_);
            av_frame_move_ref(skippedFrame_, frame_);
            return false;
        }
        av_frame_unref(skippedFrame_);
        seekTargetPts_ = AV_NOPTS_VALUE;
        return true;
    }

    // --- Planned playback: legs (decode thread) ---

    bool planLoops() const {
        return plan_.loopMode != LoopMode::ONCE && plan_.duration >= MIN_WRAP_DURATION;
    }

    void beginForward(double position) {
        phase_ = Phase::Forward;
        splicePending_ = false;
        splicePendingFrames_ = 0;
        lastForwardPts_.reset();
        seekExact(position);
    }

    // Loop, at the end of the file: feed the decoder the start of the clip straight
    // after its end, as one stream with a keyframe there, instead of draining it and
    // restarting it at the start. A restart (avcodec_flush_buffers) makes FFmpeg's H.264
    // and HEVC decoders set up again from the next keyframe; with D3D11VA that is a new
    // GPU decoder and surface pool, created one stream at a time on the shared device,
    // so 16 clips wrapping together waited ~0.5-0.8 s for their first frame (TD-30).
    // The frames still held for reordering come out first, then the new pass's; the
    // first frame whose time goes back is the new pass (see producePlanned). False when
    // this is not a Loop pass or the start is not a clean entry point: the caller drains
    // and the next pass restarts the decoder as before.
    bool spliceNextPass(AVPacket* packet) {
        if (!planned_ || phase_ != Phase::Forward || plan_.loopMode != LoopMode::LOOP || !planLoops() ||
            splicePending_ || !lastForwardPts_ || seekTargetPts_ != AV_NOPTS_VALUE) {
            return false;
        }
        // By time; MPEG-TS seeks by bisecting timestamps and can land past the first
        // frame, so there the start of the file, by byte.
        int64_t seekPts = startPts_;
        if (!testOvershootConsumed_ && options_.testSpliceSeekOvershootFrames != 0) {
            // Test hook: stand in for a demuxer whose backward seek overshoots (as
            // MPEG-TS can, see above) by asking for a later keyframe outright, so
            // readFirstFrame()'s "first" check (that the packet found really is the
            // clip's start, not just any keyframe) has a concrete case to refuse.
            seekPts = startPts_ + static_cast<int64_t>(options_.testSpliceSeekOvershootFrames) * frameTicks_;
            testOvershootConsumed_ = true;
        }
        bool clean = av_seek_frame(formatCtx_, videoStreamIndex_, seekPts, AVSEEK_FLAG_BACKWARD) >= 0 &&
                     readFirstFrame(packet);
        if (!clean && !(formatCtx_->iformat->flags & AVFMT_NO_BYTE_SEEK)) {
            clean = av_seek_frame(formatCtx_, -1, 0, AVSEEK_FLAG_BYTE) >= 0 && readFirstFrame(packet);
        }
        if (!clean) return false;
        avcodec_send_packet(codecCtx_, packet);  // room: the decoder just asked for input
        av_packet_unref(packet);
        splicePending_ = true;
        splicePendingFrames_ = 0;
        testStripNextSplicePts_ = options_.testStripPtsAfterSplice;
        return true;
    }

    // Read the next video packet into `packet` and keep it if it is the clip's first
    // frame and a clean entry point; false (nothing kept) otherwise.
    bool readFirstFrame(AVPacket* packet) {
        int rc = 0;
        while ((rc = av_read_frame(formatCtx_, packet)) >= 0 && packet->stream_index != videoStreamIndex_) {
            av_packet_unref(packet);
        }
        if (rc < 0) return false;
        const bool first = packet->pts == AV_NOPTS_VALUE || packet->pts < startPts_ + (frameTicks_ + 1) / 2;
        if (first && startsCleanly(packet)) return true;
        av_packet_unref(packet);
        return false;
    }

    // Whether decoding can start at `packet` with nothing before it: a keyframe that,
    // for H.264 and HEVC, is an IDR (or BLA) picture. Their other keyframes may be
    // followed by pictures that refer back across it (open GOP), which the end of the
    // clip cannot supply.
    bool startsCleanly(const AVPacket* packet) const {
        if (!(packet->flags & AV_PKT_FLAG_KEY)) return false;
        const AVCodecParameters* par = formatCtx_->streams[videoStreamIndex_]->codecpar;
        if (par->codec_id != AV_CODEC_ID_H264 && par->codec_id != AV_CODEC_ID_HEVC) return true;
        const bool hevc = par->codec_id == AV_CODEC_ID_HEVC;
        auto isEntry = [hevc](uint8_t header) {
            if (!hevc) return (header & 0x1f) == 5;  // IDR slice
            const int type = (header >> 1) & 0x3f;
            return type >= 16 && type <= 20;         // BLA or IDR
        };
        auto isSlice = [hevc](uint8_t header) {
            if (!hevc) return (header & 0x1f) >= 1 && (header & 0x1f) <= 5;
            return ((header >> 1) & 0x3f) < 32;      // VCL
        };
        const uint8_t* data = packet->data;
        const size_t size = static_cast<size_t>(packet->size);
        const bool annexB = size >= 4 && data[0] == 0 && data[1] == 0 &&
                            (data[2] == 1 || (data[2] == 0 && data[3] == 1));
        if (annexB) {
            for (size_t i = 0; i + 3 < size; ++i) {
                if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && isSlice(data[i + 3])) {
                    return isEntry(data[i + 3]);
                }
            }
            return false;
        }
        // Length-prefixed NAL units (MP4, Matroska): the length size is in the extradata.
        size_t lengthSize = 4;
        if (par->extradata && par->extradata_size > (hevc ? 22 : 4) && par->extradata[0] == 1) {
            lengthSize = (par->extradata[hevc ? 21 : 4] & 3) + 1;
        }
        for (size_t pos = 0; pos + lengthSize < size;) {
            size_t length = 0;
            for (size_t b = 0; b < lengthSize; ++b) length = (length << 8) | data[pos + b];
            pos += lengthSize;
            if (length == 0 || length > size - pos) return false;
            if (isSlice(data[pos])) return isEntry(data[pos]);
            pos += length;
        }
        return false;
    }

    void beginReverse(double bound) {
        phase_ = Phase::Reverse;
        splicePending_ = false;
        splicePendingFrames_ = 0;
        releaseReverse();
        building_.bound = std::min(bound, plan_.duration);
    }

    void advanceLeg() {
        leg_ = nextLeg(leg_, plan_.duration, plan_.loopMode);
        if (leg_.direction > 0) {
            beginForward(0.0);
        } else if (!tail_.empty()) {
            // The forward pass just decoded the last frames: show them backwards at
            // once instead of decoding them again (no pause at the reflection).
            phase_ = Phase::Reverse;
            releaseReverse();
            ready_ = std::move(tail_);
            readyBytes_ = tailBytes_;
            readyEnd_ = std::max(plan_.duration, framePts(ready_.back()));
            building_.bound = framePts(ready_.front());
            tail_.clear();
            tailBytes_ = 0;
        } else {
            beginReverse(plan_.duration);
        }
    }

    // Ping-Pong forward pass: keep the frames decoded since the last keyframe, so the
    // reverse leg that follows starts from them. Bounded by the reverse cache; a group
    // that does not fit is simply not kept.
    void keepTail(double pts) {
        if (plan_.loopMode != LoopMode::PING_PONG || !planLoops()) return;
        if (frame_->flags & AV_FRAME_FLAG_KEY) {
            releaseTail();
            tailOverflow_ = false;
        }
        if (tailOverflow_) return;
        const size_t bytes = frameBytes(frame_);
        AVFrame* copy = tailBytes_ + bytes <= plan_.reverseCacheBytes ? av_frame_clone(frame_) : nullptr;
        if (!copy || frame_->pts == AV_NOPTS_VALUE || (!tail_.empty() && pts <= framePts(tail_.back()))) {
            av_frame_free(&copy);
            releaseTail();
            tailOverflow_ = true;  // until the next keyframe
            return;
        }
        lastFrameBytes_ = std::max(lastFrameBytes_, bytes);
        tail_.push_back(copy);
        tailBytes_ += bytes;
        reversePeakBytes_ = std::max(reversePeakBytes_.load(), reverseBytes() + tailBytes_);
    }

    void releaseTail() {
        for (AVFrame*& frame : tail_) av_frame_free(&frame);
        tail_.clear();
        tailBytes_ = 0;
    }

    // The next frame in timeline order, or an invalid frame when there is none yet
    // (end of a Once clip, a read error, a stop request, or a stalled plan).
    VideoFrame producePlanned() {
        while (!interrupted() && !planStalled_) {
            if (phase_ == Phase::Forward) {
                if (decodeNextRaw()) {
                    if (testStripNextSplicePts_ && splicePending_ && lastForwardPts_ &&
                        frame_->pts != AV_NOPTS_VALUE &&
                        framePts(frame_) <= *lastForwardPts_ + FRAME_DUE_EPSILON) {
                        // Test hook (Finding 2): this is the exact frame that would signal
                        // the seam by its pts dropping back to the start; strip it, as a
                        // real stream that drops a timestamp right there would.
                        testStripNextSplicePts_ = false;
                        frame_->pts = AV_NOPTS_VALUE;
                    }
                    const double pts = framePts(frame_);
                    if (splicePending_) ++splicePendingFrames_;
                    // The seam is normally found by the pts dropping back to the start; a
                    // frame with no timestamp right there must not lose the reference the
                    // drop is measured against, or the splice would never resolve and every
                    // frame after it would carry the wrong (stale) leg (Finding 2). Past any
                    // real reorder depth with still no drop, force the switch by frame count.
                    const bool splicedByPts = splicePending_ && lastForwardPts_ &&
                        frame_->pts != AV_NOPTS_VALUE && pts <= *lastForwardPts_ + FRAME_DUE_EPSILON;
                    const bool splicedByCount = splicePending_ && !splicedByPts &&
                        splicePendingFrames_ > MAX_SPLICE_PENDING_FRAMES;
                    if (splicedByPts || splicedByCount) {
                        // The first frame of the pass spliced on: the last one came out whole.
                        splicePending_ = false;
                        splicePendingFrames_ = 0;
                        endSeen_ = true;
                        leg_ = nextLeg(leg_, plan_.duration, plan_.loopMode);
                    }
                    if (frame_->pts != AV_NOPTS_VALUE) {
                        lastForwardPts_ = pts;
                    } else if (!splicePending_) {
                        lastForwardPts_.reset();
                    }
                    observedEnd_ = std::max(observedEnd_.load(), pts + frameSeconds_);
                    // A frame stamped past the clip's end would show after the next pass began.
                    if (planLoops() && pts >= plan_.duration - FRAME_DUE_EPSILON) continue;
                    keepTail(pts);
                    emptyLegs_ = 0;
                    VideoFrame frame = convertFrame(frame_);
                    frame.due = frameDueTime(leg_, plan_.duration, pts, pts + frameSeconds_);
                    return frame;
                }
                if (!atEnd_) return VideoFrame{};  // read error or stop
                splicePending_ = false;  // a splice that never showed a new pass: restart as usual
                splicePendingFrames_ = 0;
                endSeen_ = true;  // observedEnd_ now includes the last frame
                if (leg_.final) {
                    phase_ = Phase::Done;          // atEnd_ stays set: the clip is over
                    return VideoFrame{};
                }
            } else if (phase_ == Phase::Reverse) {
                if (!ready_.empty() || buildChunk()) {
                    emptyLegs_ = 0;
                    return emitReverse();
                }
                if (planStalled_ || interrupted() || !building_.legEnd) return VideoFrame{};
            } else {
                return VideoFrame{};
            }
            // This pass is over: go on to the next one.
            if (++emptyLegs_ > MAX_EMPTY_LEGS) {
                stallPlan("Playback found no frames to show in the clip.");
                return VideoFrame{};
            }
            advanceLeg();
        }
        return VideoFrame{};
    }

    // --- Planned playback: the reverse leg ---

    size_t reverseBytes() const { return readyBytes_ + building_.bytes; }

    // Whether the next group can be decoded while the frame buffer is full, so the
    // reverse leg does not pause at each group boundary.
    bool canPrebuild() const {
        if (!planned_ || phase_ != Phase::Reverse || planStalled_) return false;
        if (building_.complete || ready_.empty()) return false;
        return reverseBytes() + lastFrameBytes_ <= plan_.reverseCacheBytes;
    }

    // Decode one more frame of the group being built.
    void stepBuild() {
        ReverseChunk& chunk = building_;
        if (!chunk.started) {
            if (chunk.bound <= FRAME_DUE_EPSILON) {  // nothing precedes the first frame
                chunk.complete = chunk.legEnd = true;
                return;
            }
            // A few frames early: indexes that seek by decode time would otherwise find
            // the keyframe at the bound itself (decoded before it is shown) and retry a
            // whole second back. Landing a group early costs only a trimmed decode.
            const double reorder = intraOnly_ ? 0.5 : std::max(codecCtx_->has_b_frames, 2) + 0.5;
            chunk.target = std::max(0.0, chunk.bound - reorder * frameSeconds_);
            chunk.started = true;
            if (!seekKeyframe(chunk.target)) {
                chunk.complete = chunk.legEnd = true;
                LOG_WARNING("Reverse playback: seek to {} s failed", chunk.target);
                return;
            }
        }

        if (decodeNextRaw()) {
            const double pts = framePts(frame_);
            if (frame_->pts == AV_NOPTS_VALUE) {
                stallPlan("the file has no frame timestamps to play it backwards.");
                return;
            }
            if (pts < chunk.bound - FRAME_DUE_EPSILON && (frame_->flags & AV_FRAME_FLAG_KEY) &&
                pts > landedKeyPts_ + FRAME_DUE_EPSILON) {
                // A later keyframe still before the bound: the seek landed a group early
                // (a retried seek backs off by a second or more). Only the last group is
                // needed; the frames before it belong to the next chunk.
                for (AVFrame*& old : chunk.frames) av_frame_free(&old);
                chunk.frames.clear();
                chunk.bytes = 0;
                landedKeyPts_ = pts;
            }
            if (pts < landedKeyPts_ - FRAME_DUE_EPSILON) return;  // an open GOP's leading frame
            if (pts >= chunk.bound - FRAME_DUE_EPSILON) {
                chunk.stopPts = pts;
                chunk.complete = true;
            } else {
                const size_t bytes = frameBytes(frame_);
                if (chunk.bytes + bytes > plan_.reverseCacheBytes) {
                    char at[32];
                    std::snprintf(at, sizeof(at), "%.1f s", chunk.bound);
                    stallPlan(std::string("the group of pictures before ") + at + " needs " +
                              reverseAdvice(plan_.reverseCacheBytes));
                    return;
                }
                AVFrame* copy = av_frame_clone(frame_);
                if (!copy) {
                    stallPlan("out of memory for the reverse cache.");
                    return;
                }
                // Frames of one stream share a size, so this sizes the next one too.
                lastFrameBytes_ = std::max(lastFrameBytes_, bytes);
                chunk.frames.push_back(copy);
                chunk.bytes += bytes;
                reversePeakBytes_ = std::max(reversePeakBytes_.load(), reverseBytes());
            }
        } else {
            if (interrupted()) return;
            chunk.hitEnd = atEnd_;
            if (!atEnd_) LOG_WARNING("Reverse playback: decode stopped early before {} s", chunk.bound);
            atEnd_ = false;  // the end of the file is not the end of a looping clip
            chunk.complete = true;
        }

        if (chunk.complete && chunk.frames.empty() && !chunk.legEnd) {
            // Nothing before the bound decoded from this keyframe: seek further back.
            if (chunk.target <= 0.0 || ++chunk.attempts > MAX_REVERSE_RETRIES) {
                chunk.legEnd = true;
                return;
            }
            const double back = std::max(1.0, 2.0 * (chunk.bound - chunk.target));
            chunk.target = std::max(0.0, chunk.bound - back);
            chunk.complete = false;
            chunk.stopPts = std::numeric_limits<double>::quiet_NaN();
            if (!seekKeyframe(chunk.target)) chunk.complete = chunk.legEnd = true;
        }
    }

    // Finish the group being built and make it the one emitted. False when the leg is
    // over (building_.legEnd), the plan stalled, or a stop was requested.
    bool buildChunk() {
        while (!building_.complete && !planStalled_ && !interrupted()) stepBuild();
        if (!building_.complete || building_.frames.empty()) return false;

        ReverseChunk& chunk = building_;
        double end = plan_.duration;
        if (!std::isnan(chunk.stopPts)) {
            end = std::min(chunk.stopPts, plan_.duration);
        } else if (!chunk.hitEnd) {
            end = std::min(chunk.bound, plan_.duration);
        }
        ready_ = std::move(chunk.frames);
        readyBytes_ = chunk.bytes;
        readyEnd_ = std::max(end, framePts(ready_.back()));

        const double nextBound = framePts(ready_.front());
        chunk = ReverseChunk{};
        chunk.bound = nextBound;
        return true;
    }

    VideoFrame emitReverse() {
        AVFrame* frame = ready_.back();
        ready_.pop_back();
        const double pts = framePts(frame);
        readyBytes_ -= std::min(readyBytes_, frameBytes(frame));

        VideoFrame result = convertFrame(frame);
        result.due = frameDueTime(leg_, plan_.duration, pts, readyEnd_);
        readyEnd_ = pts;
        av_frame_free(&frame);
        return result;
    }

    void releaseReverse() {
        for (AVFrame*& frame : ready_) av_frame_free(&frame);
        ready_.clear();
        readyBytes_ = 0;
        for (AVFrame*& frame : building_.frames) av_frame_free(&frame);
        building_ = ReverseChunk{};
    }

    void stallPlan(const std::string& reason) {
        planStalled_ = true;
        releaseReverse();
        releaseTail();
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (replan_) return;  // the plan that stalled was already replaced: not its error
        if (reverseError_.empty()) LOG_WARNING("Ping-Pong cannot play backwards: {}", reason);
        reverseError_ = reason;
    }

    // FFmpeg's interrupt callback (the file's AVFormatContext): non-zero ends a read.
    static int readCutRequested(void* impl) {
        const Impl* self = static_cast<const Impl*>(impl);
        if (self->options_.testBeforeFileRead) self->options_.testBeforeFileRead(self->cutReads_);
        return self->cutReads_.load(std::memory_order_relaxed) ? 1 : 0;
    }

    // Asked to stop, or to start over at a new plan: decoding toward the old one is moot.
    bool interrupted() const { return stopDecoding_ || replan_; }

    void backgroundDecodingLoop() {
        const char* failure = nullptr;
        try {
            decodeUntilStopped();
        } catch (const std::exception& e) {
            // No memory for a frame, most likely. The clip stops with an error and the
            // app runs on; the next startPlayback() starts a new thread.
            LOG_ERROR("Video decode thread stopped: {}", e.what());
            failure = "Video decoding stopped (out of memory?)";
        }
        {
            // The error and the exit in one step: a startPlayback() in between would
            // clear the error and still find the thread running.
            std::lock_guard<std::mutex> lock(bufferMutex_);
            if (failure) lastError_ = failure;
            threadExited_ = true;
        }
        bufferCondition_.notify_all();
    }

    void decodeUntilStopped() {
        {
            // Started by startIdleThread(): nothing to decode until the first plan.
            std::unique_lock<std::mutex> lock(bufferMutex_);
            bufferCondition_.wait(lock, [this] { return !awaitPlan_ || stopDecoding_ || replan_; });
            awaitPlan_ = false;
        }
        DecodeBackoff backoff;
        while (!stopDecoding_) {
            if (replan_) {
                applyPendingPlan();
                backoff = DecodeBackoff{};
            }
#ifdef _WIN32
            leaveGpu();
#endif
            bool room = true;
            {
                std::unique_lock<std::mutex> lock(bufferMutex_);
                if (canPrebuild()) {
                    room = frameBuffer_.size() < maxBufferSize_;
                } else {
                    bufferCondition_.wait(lock, [this]() {
                        return stopDecoding_ || replan_ || frameBuffer_.size() < maxBufferSize_ || mustLeaveGpu();
                    });
                }
            }

            if (stopDecoding_) break;
            if (replan_ || mustLeaveGpu()) continue;
            if (!room) {
                stepBuild();  // the buffer is full: decode the next reverse group meanwhile
                continue;
            }

            VideoFrame frame = planned_ ? producePlanned() : decodeNextFrame();
            if (stopDecoding_) break;  // interrupted mid-skip: not a decode failure
            if (replan_) continue;     // decoded for the plan just replaced: dropped
            if (!frame.isValid()) {
                std::chrono::milliseconds delay{10};  // at end of stream: idle until seek/stop
                if (!atEnd_ && !planStalled_) {
                    delay = backoff.onFailure();
                    if (backoff.failing()) recordFailure();
                }
                std::unique_lock<std::mutex> lock(bufferMutex_);
                bufferCondition_.wait_for(lock, delay, [this]() { return interrupted(); });
                continue;
            }

            backoff.onSuccess();
            if (options_.testBeforeQueueFrame) options_.testBeforeQueueFrame();
            {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                if (replan_) continue;  // raised since the check above, with the queue emptied
                lastError_.clear();
                frameBuffer_.push(std::move(frame));
            }
        }
    }

    // Zero-copy stopped and leaveGpu() has not run yet.
    bool mustLeaveGpu() const {
#ifdef _WIN32
        return zeroCopyStopped_ && !leftGpu_;
#else
        return false;
#endif
    }

    void recordFailure() {
        std::string message = "Video decoding failed";
        if (lastReadError_ < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(lastReadError_, err, sizeof(err));
            message += std::string(": ") + err;
        }
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (lastError_.empty()) LOG_ERROR("{} (retrying with backoff)", message);
        lastError_ = std::move(message);
    }

    // FFmpeg objects
    DecodeOptions options_;
    AVFormatContext* formatCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* skippedFrame_ = nullptr;  // last frame skipped on the way to a seek target
    AVFrame* rgbFrame_ = nullptr;
    std::vector<uint8_t> rgbBuffer_;
    int rgbColorKey_ = -1;  // matrix and range swsCtx_ was set up for (playback only)

    // Hardware decoding (ADR-055; decode thread, or open() before it starts)
    AVBufferRef* hwDevice_ = nullptr;         // this decoder's reference to the shared device
    std::atomic<bool> hwActive_{false};       // frames come from the GPU (read by the UI)
    int hwFrames_ = 0;                        // frames the GPU has produced
    AVBufferPool* transferPool_ = nullptr;    // memory for frames copied from the GPU
    int transferPoolBytes_ = 0;
#ifdef _WIN32
    ID3D11Texture2D* staging_ = nullptr;      // this stream's readback texture
    D3D11_TEXTURE2D_DESC stagingDesc_{};
    ID3D11Query* copyDone_ = nullptr;         // signals a GPU pool copy done (ADR-056)
    AVBufferPool* gpuPool_ = nullptr;         // textures frames are left on the GPU in (ADR-056)
    GpuPoolSpec* gpuPoolSpec_ = nullptr;      // gpuPool_'s, owned by it
    D3D11_TEXTURE2D_DESC gpuPoolDesc_{};      // the decoder surfaces the pool was made for
    std::atomic<bool> leftGpu_{false};        // leaveGpu() ran (decode thread; read by the wait)
#endif
    std::atomic<bool> zeroCopyStopped_{false};  // stopZeroCopy() was called (any thread)
    std::string zeroCopyNote_;                // why, guarded by bufferMutex_
    std::optional<double> landedKeyAfterResume_;  // landedKeyPts_ to restore after a resume
    std::atomic<int> reorderDelay_{0};        // codec reorder delay, for checkReverse()
    std::string decodeNote_;                  // why software, guarded by bufferMutex_

    // Stream indices
    int videoStreamIndex_ = -1;
    int audioStreamIndex_ = -1;

    // State
    VideoInfo info_;
    double timeBase_ = 0.0;
    int64_t startPts_ = 0;      // stream start_time, in time_base units
    int64_t frameTicks_ = 1;    // one frame's duration, in time_base units
    double frameSeconds_ = 0.04;  // one frame's duration
    bool intraOnly_ = false;      // every frame is a keyframe (ProRes, MJPEG, ...)
    std::optional<double> widestKeyGap_;  // keyframe index read at open(); see measureWidestKeyGap()
    // TD-32: an AVI B-frame stream (not H.264/HEVC) whose per-frame pts is only
    // partially back-filled by the demuxer and not trustworthy where present; every
    // frame is treated as if it had no pts (sequential timing instead).
    bool unreliableFramePts_ = false;
    // Decode thread only (seek() stops the thread first).
    int64_t seekTargetPts_ = AV_NOPTS_VALUE;  // skip frames before this; NOPTS = no seek pending
    int64_t seekFromPts_ = 0;   // where the demuxer was last asked to seek
    bool checkFirstPacket_ = false;  // next video packet is the first after a demuxer seek
    bool checkFirstFrame_ = false;   // next decoded frame is the first after a demuxer seek
    bool keepPreroll_ = false;       // a keyframe seek: keep the frames before the target
    double landedKeyPts_ = 0.0;      // pts of the keyframe the last demuxer seek landed on
    int testLateRemaining_ = 0;      // TD-14b: reachedSeekTarget() pretends "landed late"
    int testOffKeyRemaining_ = 0;    // TD-14b: decodeNextRaw() pretends "landed off-key"
    int lastReadError_ = 0;     // last non-EOF av_read_frame error (decode thread only)
    bool isOpen_ = false;
    bool isLive_ = false;   // live capture device (no seek/duration)
    // Written by the background decode thread, read on the main thread (via the getters /
    // VideoPlayer::update). Atomic to avoid a data race / UB. Single-writer-at-a-time (seek()
    // joins the decode thread first; a new plan is applied on that thread), so plain
    // load/store ordering is sufficient. The exception is atEnd_, which startPlayback()
    // also clears, under bufferMutex_.
    std::atomic<int64_t> currentFrame_{0};
    std::atomic<double> currentTime_{0.0};
    std::atomic<bool> atEnd_{false};
    std::atomic<double> observedEnd_{0.0};  // latest frame end decoded going forward
    std::atomic<bool> endSeen_{false};      // a forward pass reached the end of the stream
    std::atomic<size_t> reversePeakBytes_{0};
    std::atomic<uint64_t> restarts_{0};     // avcodec_flush_buffers calls (decoderRestarts)

    // Planned playback (decode thread: applyPendingPlan sets it up)
    bool planned_ = false;
    DecodePlan plan_;
    PlaybackLeg leg_;
    Phase phase_ = Phase::Done;
    std::atomic<bool> planStalled_{false};
    int emptyLegs_ = 0;
    ReverseChunk building_;          // group being decoded
    std::vector<AVFrame*> ready_;    // group being emitted, oldest first (emitted from the back)
    size_t readyBytes_ = 0;
    double readyEnd_ = 0.0;          // where the interval of ready_.back() ends
    size_t lastFrameBytes_ = 0;
    std::vector<AVFrame*> tail_;     // Ping-Pong forward pass: frames since the last keyframe
    size_t tailBytes_ = 0;
    bool tailOverflow_ = false;      // this group did not fit; kept again from the next keyframe
    std::string reverseError_;       // guarded by bufferMutex_
    bool splicePending_ = false;     // Loop: the start was fed after the end (spliceNextPass)
    std::optional<double> lastForwardPts_;  // the last frame decoded going forward, if timed
    int splicePendingFrames_ = 0;    // frames decoded since splicePending_ was set (Finding 2)
    bool testStripNextSplicePts_ = false;  // test hook: NOPTS the next frame after a splice
    bool testOvershootConsumed_ = false;   // test hook: testSpliceSeekOvershootFrames used once

    // Background decoding
    std::thread backgroundThread_;
    mutable std::mutex bufferMutex_;
    std::condition_variable bufferCondition_;
    std::queue<VideoFrame> frameBuffer_;
    std::string lastError_;     // guarded by bufferMutex_
    std::atomic<bool> stopDecoding_{false};
    std::atomic<bool> replan_{false};  // pendingPlan_ waits for the decode thread
    bool awaitPlan_ = false;  // the thread idles until a plan (startIdleThread); bufferMutex_
    bool threadExited_ = false;  // the decode thread's function has returned; bufferMutex_
    // How long stopBackgroundDecoding() lets the thread finish what it is reading.
    static constexpr std::chrono::milliseconds STOP_GRACE{250};
    static constexpr double MAX_DURATION_SECONDS = 1e7;  // 115 days
    std::atomic<bool> cutReads_{false};  // stopBackgroundDecoding(): the thread's reads fail at once
    DecodePlan pendingPlan_;            // guarded by bufferMutex_
    size_t maxBufferSize_ = 8;          // guarded by bufferMutex_ while the thread runs
};

#else // !QUADMAPPER_HAS_FFMPEG

// Stub implementation when FFmpeg is not available
namespace quadmapper {

class VideoDecoder::Impl {
public:
    bool open(const std::filesystem::path&, const DecodeOptions&) { return false; }
    bool openCamera(const std::string&, int, int, double) { return false; }
    void close() {}
    bool isOpen() const { return false; }
    bool isLive() const { return false; }
    const VideoInfo& getInfo() const { return info_; }
    bool seek(double) { return false; }
    VideoFrame decodeNextFrame() { return VideoFrame{}; }
    double getCurrentTime() const { return 0.0; }
    int64_t getCurrentFrame() const { return 0; }
    bool isAtEnd() const { return true; }
    void startBackgroundDecoding(size_t) {}
    void startIdleThread() {}
    void startPlayback(const DecodePlan&, size_t) {}
    std::string checkReverse(size_t) const { return {}; }
    std::string getReverseError() const { return {}; }
    size_t getReverseCachePeakBytes() const { return 0; }
    uint64_t decoderRestarts() const { return 0; }
    double getObservedEnd() const { return 0.0; }
    void stopBackgroundDecoding() {}
    VideoFrame getBufferedFrame() { return VideoFrame{}; }
    bool peekBufferedDue(double&) const { return false; }
    YuvPicture peekBufferedPicture() const { return {}; }
    bool hasBufferedFrames() const { return false; }
    size_t getBufferedFrameCount() const { return 0; }
    std::string getLastError() const { return {}; }
    bool isHardwareDecoding() const { return false; }
    std::string getDecodeNote() const { return {}; }
    bool isZeroCopy() const { return false; }
    void stopZeroCopy(const std::string&) {}
    std::string getZeroCopyNote() const { return {}; }
private:
    VideoInfo info_;
};

#endif // QUADMAPPER_HAS_FFMPEG

// Public interface implementation
VideoDecoder::VideoDecoder() : impl_(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;
VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
VideoDecoder& VideoDecoder::operator=(VideoDecoder&&) noexcept = default;

bool VideoDecoder::open(const std::filesystem::path& path, const DecodeOptions& options) {
    return impl_->open(path, options);
}
bool VideoDecoder::openCamera(const std::string& deviceName, int width, int height, double fps) {
    return impl_->openCamera(deviceName, width, height, fps);
}
void VideoDecoder::close() { impl_->close(); }
bool VideoDecoder::isOpen() const { return impl_->isOpen(); }
bool VideoDecoder::isLive() const { return impl_->isLive(); }
const VideoInfo& VideoDecoder::getInfo() const { return impl_->getInfo(); }
bool VideoDecoder::seek(double timeSeconds) { return impl_->seek(timeSeconds); }
VideoFrame VideoDecoder::decodeNextFrame() { return impl_->decodeNextFrame(); }
double VideoDecoder::getCurrentTime() const { return impl_->getCurrentTime(); }
int64_t VideoDecoder::getCurrentFrame() const { return impl_->getCurrentFrame(); }
bool VideoDecoder::isAtEnd() const { return impl_->isAtEnd(); }
void VideoDecoder::startBackgroundDecoding(size_t bufferSize) { impl_->startBackgroundDecoding(bufferSize); }
void VideoDecoder::startIdleThread() { impl_->startIdleThread(); }
void VideoDecoder::startPlayback(const DecodePlan& plan, size_t bufferSize) {
    impl_->startPlayback(plan, bufferSize);
}
std::string VideoDecoder::checkReverse(size_t cacheBytes) const { return impl_->checkReverse(cacheBytes); }
std::string VideoDecoder::getReverseError() const { return impl_->getReverseError(); }
size_t VideoDecoder::getReverseCachePeakBytes() const { return impl_->getReverseCachePeakBytes(); }
uint64_t VideoDecoder::decoderRestarts() const { return impl_->decoderRestarts(); }
double VideoDecoder::getObservedEnd() const { return impl_->getObservedEnd(); }
void VideoDecoder::stopBackgroundDecoding() { impl_->stopBackgroundDecoding(); }
VideoFrame VideoDecoder::getBufferedFrame() { return impl_->getBufferedFrame(); }
bool VideoDecoder::peekBufferedDue(double& due) const { return impl_->peekBufferedDue(due); }
YuvPicture VideoDecoder::peekBufferedPicture() const { return impl_->peekBufferedPicture(); }
bool VideoDecoder::hasBufferedFrames() const { return impl_->hasBufferedFrames(); }
size_t VideoDecoder::getBufferedFrameCount() const { return impl_->getBufferedFrameCount(); }
std::string VideoDecoder::getLastError() const { return impl_->getLastError(); }
bool VideoDecoder::isHardwareDecoding() const { return impl_->isHardwareDecoding(); }
std::string VideoDecoder::getDecodeNote() const { return impl_->getDecodeNote(); }

int VideoDecoder::hardwareDevicesCreated() {
#ifdef QUADMAPPER_HAS_FFMPEG
    SharedHardwareDevice& shared = sharedHardwareDevice();
    std::lock_guard<std::mutex> lock(shared.mutex);
    return shared.created;
#else
    return 0;
#endif
}

void* VideoDecoder::sharedD3dDevice() {
#if defined(QUADMAPPER_HAS_FFMPEG) && defined(_WIN32)
    const AVD3D11VADeviceContext* d3d = sharedD3d();
    return d3d ? d3d->device : nullptr;
#else
    return nullptr;
#endif
}

bool VideoDecoder::isZeroCopy() const { return impl_->isZeroCopy(); }
void VideoDecoder::stopZeroCopy(const std::string& reason) { impl_->stopZeroCopy(reason); }
std::string VideoDecoder::getZeroCopyNote() const { return impl_->getZeroCopyNote(); }

YuvPicture VideoDecoder::downloadPicture(const YuvPicture& picture) {
    if (!picture.onGpu()) return picture;
#if defined(QUADMAPPER_HAS_FFMPEG) && defined(_WIN32)
    const auto* frame = static_cast<const AVFrame*>(picture.owner.get());  // see shareGpuFrame()
    AVFrame* copy = av_frame_alloc();
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC stagingDesc{};
    const bool ok = frame && copy && isGpuFrame(frame) && downloadGpuFrame(frame, copy, staging, stagingDesc);
    releaseStaging(staging);
    if (!ok) {
        av_frame_free(&copy);
        return {};
    }
    YuvPicture result;
    result.layout = copy->format == AV_PIX_FMT_P010LE ? YuvLayout::P010 : YuvLayout::NV12;
    result.width = copy->width;
    result.height = copy->height;
    for (int p = 0; p < 2; ++p) {
        result.planes[p] = copy->data[p];
        result.strides[p] = copy->linesize[p];
    }
    result.matrix = picture.matrix;
    result.fullRange = picture.fullRange;
    result.owner = std::shared_ptr<const void>(copy, freeFrameHandle);
    return result;
#else
    return {};
#endif
}

long VideoDecoder::liveGpuFrames() {
#if defined(QUADMAPPER_HAS_FFMPEG) && defined(_WIN32)
    return g_liveGpuFrames.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

long VideoDecoder::liveD3dObjects() {
#if defined(QUADMAPPER_HAS_FFMPEG) && defined(_WIN32)
    return g_liveD3dObjects.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

long VideoDecoder::gpuReadbacksOnThisThread() {
#if defined(QUADMAPPER_HAS_FFMPEG) && defined(_WIN32)
    return t_gpuReadbacks;
#else
    return 0;
#endif
}

}  // namespace quadmapper
