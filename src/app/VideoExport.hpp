#pragma once

// Video export job (ADR-060). It renders the show offline into one H.264 MP4 per
// chosen output, at the output's own resolution and rotation, with the mapped videos'
// sound.
//
// How it works:
// - start() takes a snapshot of the show: the project as it would be saved, loaded
//   into a private EditorState and SourceManager. The sources play on an injected
//   clock, so the live show and its clocks are untouched.
// - Each app frame, step() runs in place of the live projector render. Frame n goes
//   like this:
//   1. move the clock to n / fps;
//   2. wait until every video has decoded the frame due then (framesCoverClock);
//   3. draw every output's frame through the live OutputManager with export
//      rendering on (output resolution, no blackout, no Identify);
//   4. read it back and hand it to an AsyncMp4Writer per output.
//   The projectors meanwhile show the frames being exported.
// - Main thread with the editor's GL context current: the constructor, step() and
//   the destructor (the snapshot's sources own GL textures).

#include "editor/VideoExportPlan.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quadmapper {

struct EditorState;
class OutputManager;
class Renderer;
class SourceManager;
class AsyncMp4Writer;
class OfflineAudioMix;

class VideoExport {
public:
    // masterGain: the app's master volume, 0 when muted.
    VideoExport(VideoExportRequest request, float masterGain);
    ~VideoExport();  // aborts unfinished files (their .part is deleted)

    VideoExport(const VideoExport&) = delete;
    VideoExport& operator=(const VideoExport&) = delete;

    // Snapshot the show and start opening its media. False with the reason.
    bool start(EditorState& live, OutputManager& outputs, SourceManager& liveSources, std::string& error);

    // One app frame of work: opening, then as many frames as fit in about 40 ms, then
    // finishing the files. `live` lends the test-grid textures of quads without media.
    void step(EditorState& live, OutputManager& outputs, Renderer& renderer);

    void cancel() { cancelRequested_ = true; }
    bool active() const { return status_.active; }
    const VideoExportStatus& status() const { return status_; }

private:
    struct Target {
        UUID outputId;
        int width = 0;
        int height = 0;
        std::filesystem::path path;
        std::unique_ptr<AsyncMp4Writer> writer;
        std::unique_ptr<OfflineAudioMix> mix;  // null: no sound track
    };

    void stepOpening(OutputManager& outputs);
    bool openTargets(OutputManager& outputs, std::string& error);
    // Moves the clock to the next frame's time and says whether every video has taken
    // the frame due then. False: one is late and `until` has passed; call again in the
    // next app frame. True also once a video has been late for the whole decode wait:
    // the frame is then drawn with the picture that video has.
    bool framesDue(std::chrono::steady_clock::time_point until);
    bool renderFrame(EditorState& live, OutputManager& outputs, Renderer& renderer, std::string& error);
    void finish();
    void fail(const std::string& message);
    void release();  // abort writers, free the snapshot
    void setClock(double seconds);

    VideoExportRequest request_;
    float masterGain_ = 1.0f;
    VideoExportStatus status_;

    enum class Phase { Idle, Opening, Rendering, Finishing };
    Phase phase_ = Phase::Idle;
    bool cancelRequested_ = false;

    std::unique_ptr<EditorState> es_;
    std::unique_ptr<SourceManager> sources_;
    std::vector<Target> targets_;

    // The injected clock, as steady_clock ticks: read by the snapshot's sources.
    std::chrono::steady_clock::time_point base_;
    std::shared_ptr<std::atomic<int64_t>> clockTicks_;

    std::chrono::steady_clock::time_point openingSince_;
    int frameCount_ = 0;
    int nextFrame_ = 0;
    int lateFrames_ = 0;  // frames on which a video missed its decode wait
    // Since when the frame about to be drawn has waited for a late video.
    std::optional<std::chrono::steady_clock::time_point> waitingSince_;
    std::vector<uint8_t> pixels_;
    std::vector<float> samples_;
};

}  // namespace quadmapper
