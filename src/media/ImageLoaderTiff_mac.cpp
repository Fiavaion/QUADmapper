#include "ImageLoaderTiffMac.hpp"
#include "core/FileSystem.hpp"
#include "core/MediaLimits.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <string>

namespace quadmapper {

namespace {

// Releases a Core Foundation object at the end of the scope.
template <typename T>
class CfRef {
public:
    explicit CfRef(T ref) : ref_(ref) {}
    ~CfRef() {
        if (ref_) CFRelease(ref_);
    }
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
    operator T() const { return ref_; }

private:
    T ref_;
};

// Colour that was stored multiplied by its alpha, divided back out. `max` is the
// largest sample value (255 or 65535).
uint32_t unpremultiply(uint32_t colour, uint32_t alpha, uint32_t max) {
    if (alpha == 0) return 0;
    return std::min(max, (colour * max + alpha / 2) / alpha);
}

// The image's own samples, when they are 8 or 16 bit grey or RGB: copied as they are,
// without colour management, which is what the Windows reader does. False for anything
// else (CMYK, a palette, 1 bit, floating point); drawRgba covers those.
bool copySamples(CGImageRef image, CFDataRef data, std::vector<uint8_t>& rgba) {
    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    const size_t bits = CGImageGetBitsPerComponent(image);
    const CGBitmapInfo info = CGImageGetBitmapInfo(image);
    if ((bits != 8 && bits != 16) || (info & kCGBitmapFloatComponents)) return false;

    const CGColorSpaceModel model = CGColorSpaceGetModel(CGImageGetColorSpace(image));
    const size_t colours = model == kCGColorSpaceModelMonochrome ? 1 : model == kCGColorSpaceModelRGB ? 3 : 0;
    if (colours == 0) return false;

    const CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);
    if (alpha == kCGImageAlphaOnly) return false;
    const bool extraSample = alpha != kCGImageAlphaNone;  // a sample beside the colours, used or not
    const bool extraFirst = alpha == kCGImageAlphaFirst || alpha == kCGImageAlphaPremultipliedFirst ||
                            alpha == kCGImageAlphaNoneSkipFirst;
    const bool hasAlpha = extraSample && alpha != kCGImageAlphaNoneSkipFirst && alpha != kCGImageAlphaNoneSkipLast;
    const bool premultiplied = alpha == kCGImageAlphaPremultipliedFirst || alpha == kCGImageAlphaPremultipliedLast;
    const size_t samples = colours + (extraSample ? 1 : 0);
    if (CGImageGetBitsPerPixel(image) != samples * bits) return false;

    const CGBitmapInfo order = info & kCGBitmapByteOrderMask;
    bool littleEndian = false;
    if (bits == 8) {
        if (order != kCGBitmapByteOrderDefault && order != kCGBitmapByteOrder32Big) return false;
    } else if (order == kCGBitmapByteOrder16Little) {
        littleEndian = true;
    } else if (order != kCGBitmapByteOrderDefault && order != kCGBitmapByteOrder16Big) {
        return false;
    }

    const size_t sampleBytes = bits / 8;
    const size_t rowBytes = CGImageGetBytesPerRow(image);
    const size_t needed = rowBytes * (height - 1) + width * samples * sampleBytes;
    if (rowBytes < width * samples * sampleBytes || static_cast<size_t>(CFDataGetLength(data)) < needed) {
        return false;
    }

    const uint8_t* bytes = CFDataGetBytePtr(data);
    const uint32_t max = bits == 8 ? 255u : 65535u;
    const auto to8 = [max](uint32_t v) { return static_cast<uint8_t>(max == 255u ? v : (v * 255u + 32767u) / 65535u); };
    rgba.resize(width * height * 4);
    uint8_t* out = rgba.data();
    for (size_t y = 0; y < height; ++y) {
        const uint8_t* p = bytes + y * rowBytes;
        for (size_t x = 0; x < width; ++x) {
            uint32_t v[4] = {0, 0, 0, 0};
            for (size_t s = 0; s < samples; ++s, p += sampleBytes) {
                v[s] = bits == 8 ? p[0]
                                 : littleEndian ? static_cast<uint32_t>(p[0] | (p[1] << 8))
                                                : static_cast<uint32_t>((p[0] << 8) | p[1]);
            }
            const uint32_t* colour = v + (extraSample && extraFirst ? 1 : 0);
            const uint32_t a = hasAlpha ? v[extraFirst ? 0 : samples - 1] : max;
            uint32_t r = colour[0];
            uint32_t g = colours == 3 ? colour[1] : r;
            uint32_t b = colours == 3 ? colour[2] : r;
            if (premultiplied) {
                r = unpremultiply(r, a, max);
                g = unpremultiply(g, a, max);
                b = unpremultiply(b, a, max);
            }
            *out++ = to8(r);
            *out++ = to8(g);
            *out++ = to8(b);
            *out++ = to8(a);
        }
    }
    return true;
}

// Any other kind of image: Core Graphics draws it into an 8-bit RGBA bitmap. A bitmap
// context holds premultiplied colour, so the alpha is divided back out afterwards.
bool drawRgba(CGImageRef image, std::vector<uint8_t>& rgba) {
    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    rgba.assign(width * height * 4, 0);
    CfRef<CGColorSpaceRef> space(CGColorSpaceCreateDeviceRGB());
    CfRef<CGContextRef> context(CGBitmapContextCreate(
        rgba.data(), width, height, 8, width * 4, space,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big));
    if (!context) return false;
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)), image);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const uint32_t a = rgba[i + 3];
        if (a == 255) continue;
        for (size_t c = 0; c < 3; ++c) rgba[i + c] = static_cast<uint8_t>(unpremultiply(rgba[i + c], a, 255));
    }
    return true;
}

}  // namespace

Result<ImageData, Error> macDecodeTiff(const std::filesystem::path& path) {
    const std::string utf8 = utf8FromPath(path);
    CfRef<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()), static_cast<CFIndex>(utf8.size()), false));
    if (!url) return Err<ImageData>("the file's name cannot be used");
    CfRef<CGImageSourceRef> source(CGImageSourceCreateWithURL(url, nullptr));
    if (!source || CGImageSourceGetCount(source) == 0) return Err<ImageData>("not a readable image file");
    CfRef<CGImageRef> image(CGImageSourceCreateImageAtIndex(source, 0, nullptr));
    if (!image) return Err<ImageData>("no image frame");

    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0) return Err<ImageData>("unreadable image size");
    // Checked before the pixels are decoded, like the stb path.
    if (const std::string tooLarge =
            pictureSizeProblem(static_cast<long long>(width), static_cast<long long>(height));
        !tooLarge.empty()) {
        return Err<ImageData>(tooLarge);
    }

    // The decode happens here; a file cut short has no data to give.
    CfRef<CFDataRef> data(CGDataProviderCopyData(CGImageGetDataProvider(image)));
    if (!data) return Err<ImageData>("pixel data could not be read");

    ImageData result;
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.channels = 4;
    if (!copySamples(image, data, result.pixels) && !drawRgba(image, result.pixels)) {
        return Err<ImageData>("unsupported pixel format");
    }
    return Ok(std::move(result));
}

}  // namespace quadmapper
