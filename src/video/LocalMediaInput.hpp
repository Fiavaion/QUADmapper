#pragma once

// Include only where FFmpeg is available (QUADMAPPER_HAS_FFMPEG).
extern "C" {
#include <libavformat/avformat.h>
}

namespace quadmapper {

// FFmpeg's protocols for a local media file. The app never opens URLs, and with only
// these a crafted file (an HLS playlist named .mp4, say) cannot make FFmpeg fetch from
// the network.
constexpr const char* LOCAL_MEDIA_PROTOCOLS = "file,crypto,data";

// avformat_open_input() on a UTF-8 file path, with only LOCAL_MEDIA_PROTOCOLS allowed.
// FFmpeg asks the interrupt callback, when one is given, before each read of the file;
// a non-zero answer ends the read or seek in progress with AVERROR_EXIT.
inline int openLocalMediaInput(AVFormatContext** ctx, const char* utf8Path,
                               const AVIOInterruptCB* interrupt = nullptr) {
    if (interrupt) {
        *ctx = avformat_alloc_context();  // freed by avformat_open_input() if it fails
        if (!*ctx) return AVERROR(ENOMEM);
        (*ctx)->interrupt_callback = *interrupt;
    }
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", LOCAL_MEDIA_PROTOCOLS, 0);
    const int result = avformat_open_input(ctx, utf8Path, nullptr, &options);
    av_dict_free(&options);
    return result;
}

}  // namespace quadmapper
