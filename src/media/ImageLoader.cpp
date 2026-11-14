#include "ImageLoader.hpp"
#include "core/FileSystem.hpp"
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
    std::string ext = utf8FromPath(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool isTiff(const std::filesystem::path& path) {
    const std::string ext = lowercaseExtension(path);
    return ext == ".tif" || ext == ".tiff";
}

// stbi_load() opens its filename in the ANSI code page, so a non-ASCII folder fails.
// Read the bytes through the wide path and decode from memory instead.
unsigned char* stbiLoadPath(const std::filesystem::path& path, int* width, int* height,
                            int* channels, int desiredChannels) {
    auto bytes = FileSystem::readBinaryFile(path);
    if (!bytes || bytes.value().empty() ||
        bytes.value().size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return nullptr;
    }
    return stbi_load_from_memory(bytes.value().data(), static_cast<int>(bytes.value().size()),
                                 width, height, channels, desiredChannels);
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
// converted to straight-alpha RGBA, top row first like the stb path. desiredChannels 0
// keeps RGBA.
Result<ImageData, Error> loadTiff(const std::filesystem::path& path, int desiredChannels) {
    using Microsoft::WRL::ComPtr;
    const std::string name = utf8FromPath(path.filename());
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

// Grey (1) and grey+alpha (2) images become RGBA: a GL_R8 / GL_RG8 texture would sample
// as red / red-green in the quad shader. Grey goes to R, G and B; alpha is kept or 255.
void expandGreyToRgba(ImageData& image) {
    if (image.channels != 1 && image.channels != 2) return;
    const size_t pixelCount = static_cast<size_t>(image.width) * image.height;
    const size_t stride = static_cast<size_t>(image.channels);
    std::vector<uint8_t> rgba(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint8_t grey = image.pixels[i * stride];
        rgba[i * 4 + 0] = grey;
        rgba[i * 4 + 1] = grey;
        rgba[i * 4 + 2] = grey;
        rgba[i * 4 + 3] = stride == 2 ? image.pixels[i * stride + 1] : 255;
    }
    image.pixels = std::move(rgba);
    image.channels = 4;
}

} // namespace

Result<ImageData, Error> ImageLoader::load(const std::filesystem::path& path) {
    // Check if file exists
    if (!std::filesystem::exists(path)) {
        return Err<ImageData>("File not found: " + utf8FromPath(path));
    }

    if (isTiff(path)) {
        auto result = loadTiff(path, 0);
        if (result.isError()) {
            Logger::error("Failed to load image: {} - {}", utf8FromPath(path), result.error().message);
        }
        return result;
    }

    // Top row first, like every other frame source. The flag is per thread so another
    // stb user elsewhere cannot change it under us.
    stbi_set_flip_vertically_on_load_thread(false);

    // Load image data
    int width, height, channels;
    unsigned char* data = stbiLoadPath(path, &width, &height, &channels, 0);

    if (!data) {
        const char* reason = stbi_failure_reason();
        Logger::error("Failed to load image: {} - {}", utf8FromPath(path), reason ? reason : "unknown");
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
    expandGreyToRgba(result);

    Logger::debug("Image loaded: {} ({}x{}, {} channels)",
                  utf8FromPath(path.filename()), width, height, result.channels);

    return Ok(std::move(result));
}

Result<ImageData, Error> ImageLoader::loadWithChannels(
    const std::filesystem::path& path,
    int desiredChannels)
{
    // Check if file exists
    if (!std::filesystem::exists(path)) {
        return Err<ImageData>("File not found: " + utf8FromPath(path));
    }

    // Validate desired channels
    if (desiredChannels < 0 || desiredChannels > 4) {
        return Err<ImageData>("Invalid channel count: " + std::to_string(desiredChannels));
    }

    if (isTiff(path)) {
        return loadTiff(path, desiredChannels);
    }

    // Top row first, like every other frame source. The flag is per thread so another
    // stb user elsewhere cannot change it under us.
    stbi_set_flip_vertically_on_load_thread(false);

    // Load with requested channel count (0 = auto-detect)
    int width, height, channels;
    unsigned char* data = stbiLoadPath(path, &width, &height, &channels, desiredChannels);

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
    if (desiredChannels == 0) expandGreyToRgba(result);

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
