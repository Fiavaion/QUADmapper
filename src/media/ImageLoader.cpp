#include "ImageLoader.hpp"
#include "core/Logger.hpp"
#include <stb_image.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

namespace quadmapper {

namespace {

std::string lowercaseExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool isTiff(const std::filesystem::path& path) {
    const std::string ext = lowercaseExtension(path);
    return ext == ".tif" || ext == ".tiff";
}

std::string hresultText(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// Initialises COM on the calling thread for the lifetime of the scope. A thread that
// already joined another apartment (RPC_E_CHANGED_MODE) can still use WIC but must not
// be uninitialised by us, so only a successful init is paired with CoUninitialize.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool usable() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
    HRESULT result() const { return hr_; }

private:
    HRESULT hr_;
};

// Converts tightly packed RGBA to 1-3 channels with stb_image's luminance weights, so
// both decode paths agree on what a forced channel count means.
std::vector<uint8_t> convertRgba(const std::vector<uint8_t>& rgba, size_t pixelCount,
                                 int channels) {
    std::vector<uint8_t> out(pixelCount * static_cast<size_t>(channels));
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint8_t* src = &rgba[i * 4];
        uint8_t* dst = &out[i * static_cast<size_t>(channels)];
        const auto luma = static_cast<uint8_t>((src[0] * 77 + src[1] * 150 + src[2] * 29) >> 8);
        switch (channels) {
            case 1: dst[0] = luma; break;
            case 2: dst[0] = luma; dst[1] = src[3]; break;
            default: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; break;
        }
    }
    return out;
}

// Decodes the first frame of a TIFF through the Windows Imaging Component (stb_image
// has no TIFF reader). Every source format (8/16-bit gray, RGB, RGBA, CMYK...) is
// converted to straight-alpha RGBA, then rows are stored bottom-first to match the stb
// path's flip-on-load. desiredChannels 0 keeps RGBA.
Result<ImageData, Error> loadTiff(const std::filesystem::path& path, int desiredChannels) {
    using Microsoft::WRL::ComPtr;
    const std::string name = path.filename().string();
    auto fail = [&name](const std::string& reason) {
        return Err<ImageData>("Failed to decode TIFF '" + name + "': " + reason);
    };
    auto failHr = [&fail](const char* reason, HRESULT hr) {
        return fail(std::string(reason) + " (HRESULT " + hresultText(hr) + ")");
    };

    ComScope com;  // declared first so it outlives every COM pointer below
    if (!com.usable()) return failHr("COM initialisation failed", com.result());

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return failHr("WIC is unavailable", hr);

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return failHr("not a readable image file", hr);

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return failHr("no image frame", hr);

    ComPtr<IWICBitmapSource> rgbaSource;
    hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &rgbaSource);
    if (FAILED(hr)) return failHr("unsupported pixel format", hr);

    UINT width = 0;
    UINT height = 0;
    hr = rgbaSource->GetSize(&width, &height);
    if (FAILED(hr)) return failHr("unreadable image size", hr);

    // Same per-axis cap as stb_image; the byte count must also fit CopyPixels' UINT.
    constexpr UINT MAX_DIMENSION = 1u << 24;
    const unsigned long long stride = static_cast<unsigned long long>(width) * 4;
    const unsigned long long byteCount = stride * height;
    if (width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION ||
        byteCount > std::numeric_limits<UINT>::max()) {
        return fail("unsupported size " + std::to_string(width) + "x" +
                    std::to_string(height));
    }

    std::vector<uint8_t> rgba(static_cast<size_t>(byteCount));
    hr = rgbaSource->CopyPixels(nullptr, static_cast<UINT>(stride),
                                static_cast<UINT>(byteCount), rgba.data());
    if (FAILED(hr)) return failHr("pixel data could not be read", hr);

    // WIC rows are top row first; swap them in place into bottom-row-first order.
    const size_t rowBytes = static_cast<size_t>(stride);
    for (size_t top = 0, bottom = height - 1; top < bottom; ++top, --bottom) {
        std::swap_ranges(rgba.begin() + top * rowBytes, rgba.begin() + (top + 1) * rowBytes,
                         rgba.begin() + bottom * rowBytes);
    }

    ImageData result;
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.channels = desiredChannels != 0 ? desiredChannels : 4;
    result.pixels = result.channels == 4
        ? std::move(rgba)
        : convertRgba(rgba, static_cast<size_t>(width) * height, result.channels);

    Logger::debug("Image loaded: {} ({}x{}, {} channels, WIC)",
                  name, width, height, result.channels);
    return Ok(std::move(result));
}

} // namespace

Result<ImageData, Error> ImageLoader::load(const std::filesystem::path& path) {
    // Check if file exists
    if (!std::filesystem::exists(path)) {
        return Err<ImageData>("File not found: " + path.string());
    }

    if (isTiff(path)) {
        auto result = loadTiff(path, 0);
        if (result.isError()) {
            Logger::error("Failed to load image: {} - {}", path.string(), result.error().message);
        }
        return result;
    }

    // Set stb_image to flip vertically (OpenGL expects bottom-left origin)
    stbi_set_flip_vertically_on_load(true);

    // Load image data
    int width, height, channels;
    unsigned char* data = stbi_load(path.string().c_str(), &width, &height, &channels, 0);

    if (!data) {
        const char* reason = stbi_failure_reason();
        Logger::error("Failed to load image: {} - {}", path.string(), reason ? reason : "unknown");
        return Err<ImageData>("Failed to load image: " + std::string(reason ? reason : "unknown"));
    }

    // Create ImageData and copy pixels
    ImageData result;
    result.width = width;
    result.height = height;
    result.channels = channels;

    size_t dataSize = static_cast<size_t>(width) * height * channels;
    result.pixels.resize(dataSize);
    std::memcpy(result.pixels.data(), data, dataSize);

    // Free stb_image's data
    stbi_image_free(data);

    Logger::debug("Image loaded: {} ({}x{}, {} channels)",
                  path.filename().string(), width, height, channels);

    return Ok(std::move(result));
}

Result<ImageData, Error> ImageLoader::loadWithChannels(
    const std::filesystem::path& path,
    int desiredChannels)
{
    // Check if file exists
    if (!std::filesystem::exists(path)) {
        return Err<ImageData>("File not found: " + path.string());
    }

    // Validate desired channels
    if (desiredChannels < 0 || desiredChannels > 4) {
        return Err<ImageData>("Invalid channel count: " + std::to_string(desiredChannels));
    }

    if (isTiff(path)) {
        return loadTiff(path, desiredChannels);
    }

    // Set stb_image to flip vertically
    stbi_set_flip_vertically_on_load(true);

    // Load with requested channel count (0 = auto-detect)
    int width, height, channels;
    unsigned char* data = stbi_load(
        path.string().c_str(),
        &width, &height, &channels,
        desiredChannels
    );

    if (!data) {
        const char* reason = stbi_failure_reason();
        return Err<ImageData>("Failed to load image: " + std::string(reason ? reason : "unknown"));
    }

    // Use requested channels if specified, otherwise use native
    int actualChannels = (desiredChannels != 0) ? desiredChannels : channels;

    ImageData result;
    result.width = width;
    result.height = height;
    result.channels = actualChannels;

    size_t dataSize = static_cast<size_t>(width) * height * actualChannels;
    result.pixels.resize(dataSize);
    std::memcpy(result.pixels.data(), data, dataSize);

    stbi_image_free(data);

    return Ok(std::move(result));
}

bool ImageLoader::isSupported(const std::filesystem::path& path) {
    const std::string ext = lowercaseExtension(path);

    // Supported formats (.tif/.tiff decode through WIC, the rest through stb_image)
    static const std::vector<std::string> supportedExtensions = {
        ".png", ".jpg", ".jpeg", ".bmp", ".gif",
        ".tga", ".psd", ".hdr", ".pic", ".ppm", ".pgm", ".tif", ".tiff"
    };

    for (const auto& supported : supportedExtensions) {
        if (ext == supported) {
            return true;
        }
    }

    return false;
}

} // namespace quadmapper
