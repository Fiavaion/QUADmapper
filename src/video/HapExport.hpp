#pragma once

// Convert to HAP (QM-082): re-encode a video file as HAP or HAP Q in a QuickTime
// .mov, which HapFileSource then plays as compressed textures (ADR-044). HAP frames
// are all keyframes and cheap to decode, so a converted clip runs backwards exactly
// in Ping-Pong and many play at once.
//
// Frame times, frame rate and duration are kept. Sound is copied when a .mov can hold
// it, otherwise re-encoded to AAC; if neither works it is left out and the result says
// why. The output is written under a temporary name and renamed when complete, so a
// failed or cancelled conversion leaves no partial file.

#include "HapFrameEncoder.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace quadmapper {

struct HapExportResult {
    enum class Status { Done, Cancelled, Failed };
    Status status = Status::Failed;
    std::filesystem::path output;  // Done: the file written
    std::string error;             // Failed: why
    std::string audioNote;         // what happened to the sound; empty if the source had none
    int64_t frames = 0;            // video frames written
};

/// Called from the converting thread with the fraction done, 0..1.
using HapExportProgress = std::function<void(double)>;

/// "<stem>_hap.mov" (HAP) or "<stem>_hapq.mov" (HAP Q) beside the source, with
/// " 2", " 3", ... added to the stem if that name is taken.
std::filesystem::path hapOutputPath(const std::filesystem::path& source, hap::HapVariant variant);

/// The first of `desired`, "<stem> 2<ext>", "<stem> 3<ext>", ... that does not exist.
std::filesystem::path firstFreePath(const std::filesystem::path& desired);

/// Convert `source` to HAP. Blocks until done; `cancel` stops it between frames. Writes
/// `output`, or the first free name after it (firstFreePath) if it exists by the end.
/// Block compression uses `threads` threads.
HapExportResult convertToHap(const std::filesystem::path& source, const std::filesystem::path& output,
                             hap::HapVariant variant, int threads, const std::atomic<bool>& cancel,
                             const HapExportProgress& progress = {});

/// One conversion on its own thread, for the UI to poll without blocking rendering.
/// Destroying it cancels the conversion and waits for the thread.
class HapConversion {
public:
    HapConversion(std::filesystem::path source, hap::HapVariant variant);
    ~HapConversion();

    HapConversion(const HapConversion&) = delete;
    HapConversion& operator=(const HapConversion&) = delete;

    const std::filesystem::path& source() const { return source_; }
    const std::filesystem::path& plannedOutput() const { return plannedOutput_; }
    hap::HapVariant variant() const { return variant_; }

    double progress() const { return progress_.load(); }
    void cancel() { cancel_ = true; }
    bool finished() const { return finished_.load(); }
    /// Valid once finished() is true.
    const HapExportResult& result() const { return result_; }

    /// Threads block compression gets: half the cores, so playback keeps the rest.
    static int workerThreads();

private:
    std::filesystem::path source_;
    std::filesystem::path plannedOutput_;
    hap::HapVariant variant_;
    std::atomic<double> progress_{0.0};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    HapExportResult result_;  // written by the thread before finished_ is set
    std::thread thread_;
};

} // namespace quadmapper
