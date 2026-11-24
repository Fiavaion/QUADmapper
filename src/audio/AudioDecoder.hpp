#pragma once

#include <filesystem>
#include <memory>

namespace quadmapper {

struct AudioFrame;
struct AudioInfo;

/**
 * @brief Decodes the audio stream of a media file with FFmpeg
 *
 * Output is interleaved float. open() can resample and remix to a fixed format (the
 * mixer's 48 kHz stereo); multichannel sources are downmixed by swresample with the
 * standard matrix (centre and surrounds at -3 dB, LFE left out).
 *
 * Timestamps are clip seconds on the same scale as the file's video: 0 is the first
 * video stream's start time (the audio stream's own start for audio-only files), so
 * frame.pts == 2.0 is the sound that belongs to the video frame at 2.0 s.
 *
 * Not thread-safe: one thread uses a decoder at a time. The FFmpeg headers stay behind
 * the pimpl.
 */
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    AudioDecoder(AudioDecoder&&) noexcept;
    AudioDecoder& operator=(AudioDecoder&&) noexcept;

    /**
     * @brief Open the first audio stream of a file
     * @param outSampleRate Resample to this rate (0 = keep the stream's)
     * @param outChannels   Remix to this many channels, 1 or 2 (0 = keep the stream's)
     * @return false if the file cannot be opened or has no decodable audio stream
     */
    bool open(const std::filesystem::path& path, int outSampleRate = 0, int outChannels = 0);

    void close();
    bool isOpen() const;

    /**
     * @brief Give up on the file: the read or seek in progress and every later one fail
     *
     * The one call another thread may make while the decoder is in use. It is for
     * stopping a decode thread whose file has stopped answering (a network share that
     * went away). The decoder cannot be used again afterwards.
     */
    void interrupt();

    /** The stream as stored (channels, rate, codec, duration). */
    const AudioInfo& getInfo() const;

    int getOutputSampleRate() const;
    int getOutputChannels() const;

    /**
     * @brief Continue decoding from `timeSeconds` (clip time)
     *
     * Lands about 0.1 s before the target (codec pre-roll: the first packet after a
     * seek decodes wrongly), so the next frames start early; callers that need the
     * exact sample trim by frame.pts.
     */
    bool seek(double timeSeconds);

    /** The next decoded frame, or an invalid frame at the end of the stream or on error. */
    AudioFrame decodeNextFrame();

    /** Clip time of the last decoded frame (or the last seek target). */
    double getCurrentTime() const;

    /** True after the last frame of the stream was returned. */
    bool isAtEnd() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quadmapper
