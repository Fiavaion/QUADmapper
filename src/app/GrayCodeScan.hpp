#pragma once

#include "../core/Types.hpp"
#include "../core/UUID.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace quadmapper {

// "Scan Output" (ADR-053 §3): show each Gray-code frame fullscreen on one output, wait
// for it to settle, save one camera frame, and so on to the last. The state machine is
// ticked once per frame and never blocks; the application performs what it asks through
// ScanHost and ScanCamera, so tests drive it without GL, outputs or a camera. The
// sidecar writes the frames before the scan (/graycode/patterns) and decodes the
// captures after it (/graycode); AiAssist makes those calls.

// An output as the scan sees it: its display, and the size it shows there in perform
// mode (the display's mode, not the window's size, which windowed and fullscreen
// differ in). A change to either ends the scan, and makes a map taken before it stale.
struct ScanOutput {
    IVec2 resolution{0, 0};
    int monitor = -1;

    bool operator==(const ScanOutput& o) const {
        return resolution == o.resolution && monitor == o.monitor;
    }
    bool operator!=(const ScanOutput& o) const { return !(*this == o); }
};

// One camera kept open for the whole scan: opening one takes about a second, too slow
// for each of ~46 frames.
class ScanCamera {
public:
    enum class State {
        Opening,    // not yet delivering frames
        Ready,      // idle: capture() may be called
        Capturing,  // writing the frame asked for
        Failed      // error() says why; the scan ends
    };
    virtual ~ScanCamera() = default;
    virtual State state() const = 0;
    // Ready only: save a frame that arrives after this call to `pngPath` (so it shows
    // what was on the output when it was called). state() reads Capturing, then Ready
    // once it is written.
    virtual void capture(const std::string& pngPath) = 0;
    virtual std::string error() const = 0;
};

// The first camera, opened and pumped on a thread of its own until destroyed. Its
// destructor stops that thread and closes the camera.
std::unique_ptr<ScanCamera> openScanCamera();

// What the scan reads from, and does to, the application.
class ScanHost {
public:
    virtual ~ScanHost() = default;
    virtual bool locked() const = 0;    // the perform lock
    virtual bool blackout() const = 0;
    virtual bool inPerformMode() const = 0;
    virtual bool enterPerformMode() = 0;  // false: no output went fullscreen
    virtual void exitPerformMode() = 0;
    // The output now, or nullopt when it is gone.
    virtual std::optional<ScanOutput> output(const UUID& outputId) const = 0;
    // Ask the sidecar for the frames at `size` (/graycode/patterns); the reply comes
    // back through patternsReplied. False: it could not be asked.
    virtual bool requestPatterns(IVec2 size) = 0;
    // Show frame `index` of the sequence fullscreen on the scanned output from now on
    // (the other outputs black). False: its image could not be loaded.
    virtual bool showFrame(int index) = 0;
    // Back to the show on every output.
    virtual void hideFrames() = 0;
};

class GrayCodeScan {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr double DEFAULT_SETTLE_SECONDS = 0.25;
    // Added before the first frame: the switch to fullscreen, and the camera's exposure
    // adjusting to the projected light.
    static constexpr double FIRST_FRAME_EXTRA_SECONDS = 0.75;
    // A frame counts as shown after this many ticks with it up (it was drawn and
    // presented, not only set).
    static constexpr int MIN_TICKS_SHOWN = 2;

    enum class Phase {
        Preparing,  // going fullscreen, then waiting for the sidecar's frames and the camera
        Settling,   // a frame is up; waiting for it to settle
        Capturing,  // the camera is saving it
        Captured,   // every frame saved and everything restored: decode next
        Ended       // cancelled or failed (endStatus) and everything restored
    };

    // `folder` holds pattern_NNN.png (the sidecar's) and receives capture_NNN.png.
    GrayCodeScan(ScanHost& host, std::unique_ptr<ScanCamera> camera, const UUID& outputId,
                 const ScanOutput& output, std::filesystem::path folder,
                 double settleSeconds = DEFAULT_SETTLE_SECONDS);
    // Restores nothing: end it first (cancel) while the host is still valid.
    ~GrayCodeScan();

    GrayCodeScan(const GrayCodeScan&) = delete;
    GrayCodeScan& operator=(const GrayCodeScan&) = delete;

    // The frames were asked for and their reply has not come.
    bool awaitingPatterns() const {
        return phase_ == Phase::Preparing && patternsRequested_ && frameCount_ == 0;
    }
    // The reply: the sidecar wrote `frameCount` frames (from its generator; never assumed
    // here). With nullopt (the request failed) or too few, the scan ends with
    // `failStatus`.
    void patternsReplied(std::optional<int> frameCount, const std::string& failStatus = {});
    // End now with `status`, restoring what the scan changed. No-op once over.
    void fail(const std::string& status);
    void cancel() { fail("Scan cancelled"); }

    // Once a frame, after it was presented.
    void tick(Clock::time_point now);

    Phase phase() const { return phase_; }
    bool isOver() const { return phase_ == Phase::Captured || phase_ == Phase::Ended; }
    const std::string& endStatus() const { return endStatus_; }
    int frameCount() const { return frameCount_; }
    int capturedFrames() const { return captured_; }
    // "Scan: 12/46 (Esc cancels)", or "Scan: preparing... (Esc cancels)".
    std::string progress() const;

    const UUID& outputId() const { return outputId_; }
    const ScanOutput& output() const { return output_; }
    // The size the frames were made for, which the decode must use.
    IVec2 patternSize() const { return patternSize_; }
    const std::filesystem::path& folder() const { return folder_; }

    static std::string patternName(int index);
    static std::string captureName(int index);

private:
    void requestPatterns();
    void show(int index, Clock::time_point now);
    void restore();
    // Why the scan cannot go on now, or empty.
    std::string interruption() const;

    ScanHost& host_;
    std::unique_ptr<ScanCamera> camera_;
    UUID outputId_;
    ScanOutput output_;
    std::filesystem::path folder_;
    double settleSeconds_;

    Phase phase_ = Phase::Preparing;
    std::string endStatus_;
    int frameCount_ = 0;
    int index_ = 0;     // the frame up now
    int captured_ = 0;
    int ticksShown_ = 0;
    Clock::time_point shownAt_{};
    IVec2 patternSize_{0, 0};
    bool patternsRequested_ = false;
    bool enteredPerform_ = false;  // the scan put the outputs fullscreen, so it undoes it
    bool performSeen_ = false;     // perform mode was on for the scan
    bool frameShown_ = false;
};

}  // namespace quadmapper
