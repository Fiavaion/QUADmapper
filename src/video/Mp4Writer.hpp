#pragma once

// Export video: writes rendered RGBA frames (and optional mixed sound) to an H.264 MP4
// that a standalone media player can play. GL-free; the caller reads pixels back and
// hands them over.
//
// Encoder: h264_mf (Windows Media Foundation, hardware if it opens, else software),
// then libopenh264, then MPEG-4 Part 2 as a last resort. Constant frame rate: frame n
// is shown at n / fps. Colour: BT.709 limited range at 1280x720 and up, BT.601 limited
// range below, tagged on the stream. Sound: AAC, padded with silence to the video's
// length at finish (so a track with nothing written is still valid).
//
// The file is written as "<path>.part" and renamed to `path` by finish(), replacing a
// file of that name; abort() or a failure deletes the .part.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace quadmapper {

struct Mp4WriterOptions {
    int width = 0;   // size of the frames handed in; odd sizes lose the last column / row
    int height = 0;
    int fps = 30;
    int64_t videoBitRate = 0;  // 0 = automatic: 0.12 bits per pixel per frame, at least 2 Mb/s
    bool audio = false;        // add an AAC track
    int sampleRate = 48000;
    int channels = 2;          // 1 or 2
};

/// Single-threaded: one thread makes every call (open to finish / abort).
class Mp4Writer {
public:
    Mp4Writer();
    ~Mp4Writer();  // aborts if not finished

    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    /// Opens "<path>.part" for writing. Error text on failure.
    bool open(const std::filesystem::path& path, const Mp4WriterOptions& options, std::string& error);
    /// One frame of options.width x options.height RGBA8 pixels, `stride` bytes per row;
    /// bottomUp = rows stored bottom row first (GL readback).
    bool writeVideoFrame(const uint8_t* rgba, int stride, bool bottomUp);
    /// Interleaved float PCM in [-1, 1], `frames` sample frames of options.channels.
    bool writeAudio(const float* interleaved, int frames);
    /// Flushes the encoders, writes the trailer and renames the .part to the path given
    /// to open(). The final path goes in `written`.
    bool finish(std::filesystem::path& written, std::string& error);
    /// Closes and deletes the .part. Safe to call any time; idempotent.
    void abort();

    bool isOpen() const;
    /// The video encoder in use, e.g. "h264_mf"; empty before open().
    const std::string& encoderName() const;
    /// Why the last write failed (writes return false after the first failure).
    const std::string& lastError() const;
    int64_t framesWritten() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Mp4Writer on its own thread. writeVideoFrame copies the frame into a bounded queue
/// (blocks while `queueFrames` frames wait, never drops); audio is queued the same way
/// (up to about a second). Every FFmpeg call, open() included, runs on the worker, so
/// Media Foundation objects stay on one thread. finish() drains the queue and joins.
/// After a worker failure, writes return false at once and error() says why.
///
/// One producer thread makes the calls; error() may be read from any thread.
class AsyncMp4Writer {
public:
    explicit AsyncMp4Writer(size_t queueFrames = 4);
    ~AsyncMp4Writer();  // aborts if not finished

    AsyncMp4Writer(const AsyncMp4Writer&) = delete;
    AsyncMp4Writer& operator=(const AsyncMp4Writer&) = delete;

    bool open(const std::filesystem::path& path, const Mp4WriterOptions& options, std::string& error);
    bool writeVideoFrame(const uint8_t* rgba, int stride, bool bottomUp);
    bool writeAudio(const float* interleaved, int frames);
    bool finish(std::filesystem::path& written, std::string& error);
    void abort();

    const std::string& encoderName() const { return encoderName_; }  // set by open()
    std::string error() const;

private:
    struct Item {
        bool video = false;
        bool bottomUp = false;
        std::vector<uint8_t> pixels;  // tightly packed rows, as handed in
        std::vector<float> samples;
    };
    enum class Command { None, Finish, Abort };

    void run(std::filesystem::path path, Mp4WriterOptions options);
    void fail(const std::string& message);  // with mutex_ held
    bool push(Item item);
    void stop(Command command);

    const size_t queueFrames_;
    size_t audioCapacity_ = 0;  // sample frames
    int width_ = 0;
    int height_ = 0;
    int channels_ = 0;
    std::string encoderName_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> queue_;
    std::vector<std::vector<uint8_t>> spare_;  // frame buffers to reuse
    size_t queuedFrames_ = 0;
    size_t queuedSamples_ = 0;
    bool opened_ = false;   // open() has its answer
    bool openOk_ = false;
    bool failed_ = false;
    Command command_ = Command::None;
    std::string error_;
    std::filesystem::path written_;
    std::thread worker_;
};

}  // namespace quadmapper
