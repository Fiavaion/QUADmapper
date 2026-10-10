#include "GrayCodeScan.hpp"
#include "../core/FileSystem.hpp"
#include "../core/Logger.hpp"
#include "../source/CameraDevices.hpp"
#include "../source/CameraSource.hpp"

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <utility>

namespace quadmapper {

namespace {

// The camera thread gives up on a camera that sends no frame for this long.
constexpr auto FRAME_TIMEOUT = std::chrono::seconds(3);
// Between captures the thread drains the camera at about this rate, so a capture never
// starts from a stale backlog.
constexpr auto PUMP_INTERVAL = std::chrono::milliseconds(10);
// The destructor waits this long for the thread (inside a slow camera open, say), then
// lets it finish on its own share of the state.
constexpr auto STOP_WAIT = std::chrono::seconds(2);

// State shared by the camera thread and its owner.
struct CameraShared {
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable exited;
    ScanCamera::State state = ScanCamera::State::Opening;
    std::string error;
    std::string pending;  // the PNG asked for, empty when idle
    bool stop = false;
    bool done = false;
};

void runCamera(const std::shared_ptr<CameraShared>& shared) {
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->state = ScanCamera::State::Failed;
        shared->error = why;
    };
    auto stopping = [&] {
        std::lock_guard<std::mutex> lock(shared->mutex);
        return shared->stop;
    };
    // A frame newer than now: the old ones are drained and dropped first.
    auto freshFrame = [&](CameraSource& camera) {
        camera.update(0.0);
        camera.markFrameConsumed();
        const auto deadline = std::chrono::steady_clock::now() + FRAME_TIMEOUT;
        while (!camera.hasNewFrame()) {
            if (std::chrono::steady_clock::now() > deadline || stopping()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            camera.update(0.0);
        }
        return true;
    };

    const auto cameras = enumerateCameraDevices();
    if (cameras.empty()) {
        fail("no camera found");
    } else {
        CameraSource camera(cameras.front().name);
        if (!camera.initialize()) {
            fail("failed to open the camera");
        } else if (!freshFrame(camera)) {
            if (!stopping()) fail("the camera sends no frames");
        } else {
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->state = ScanCamera::State::Ready;
            }
            for (;;) {
                std::string path;
                {
                    std::unique_lock<std::mutex> lock(shared->mutex);
                    shared->wake.wait_for(lock, PUMP_INTERVAL,
                                          [&] { return shared->stop || !shared->pending.empty(); });
                    if (shared->stop) break;
                    path = shared->pending;
                }
                if (path.empty()) {
                    camera.update(0.0);  // keep the backlog drained
                    continue;
                }
                const bool ok = freshFrame(camera) &&
                                camera.captureCurrentFrameToPng(pathFromUtf8(path));
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->pending.clear();
                if (ok) {
                    shared->state = ScanCamera::State::Ready;
                } else if (!shared->stop) {
                    shared->state = ScanCamera::State::Failed;
                    shared->error = "the camera capture failed";
                    break;
                }
            }
        }
        camera.shutdown();
    }
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->done = true;
    }
    shared->exited.notify_all();
}

class ThreadedScanCamera : public ScanCamera {
public:
    ThreadedScanCamera() : shared_(std::make_shared<CameraShared>()) {
        thread_ = std::thread([shared = shared_] { runCamera(shared); });
    }

    ~ThreadedScanCamera() override {
        bool done;
        {
            std::unique_lock<std::mutex> lock(shared_->mutex);
            shared_->stop = true;
            shared_->wake.notify_all();
            done = shared_->exited.wait_for(lock, STOP_WAIT, [this] { return shared_->done; });
        }
        if (done) {
            thread_.join();
        } else {
            Logger::warning("Scan: the camera was still opening; leaving it to close");
            thread_.detach();
        }
    }

    State state() const override {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return shared_->state;
    }

    void capture(const std::string& pngPath) override {
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (shared_->state != State::Ready) return;
            shared_->pending = pngPath;
            shared_->state = State::Capturing;
        }
        shared_->wake.notify_all();
    }

    std::string error() const override {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return shared_->error;
    }

private:
    std::shared_ptr<CameraShared> shared_;
    std::thread thread_;
};

std::string numbered(const char* format, int index) {
    char name[32];
    std::snprintf(name, sizeof(name), format, index);
    return name;
}

}  // namespace

std::unique_ptr<ScanCamera> openScanCamera() {
    return std::make_unique<ThreadedScanCamera>();
}

GrayCodeScan::GrayCodeScan(ScanHost& host, std::unique_ptr<ScanCamera> camera,
                           const UUID& outputId, const ScanOutput& output,
                           std::filesystem::path folder, double settleSeconds)
    : host_(host),
      camera_(std::move(camera)),
      outputId_(outputId),
      output_(output),
      folder_(std::move(folder)),
      settleSeconds_(settleSeconds) {}

GrayCodeScan::~GrayCodeScan() = default;

std::string GrayCodeScan::patternName(int index) { return numbered("pattern_%03d.png", index); }
std::string GrayCodeScan::captureName(int index) { return numbered("capture_%03d.png", index); }

std::string GrayCodeScan::progress() const {
    if (phase_ == Phase::Preparing) return "Scan: preparing... (Esc cancels)";
    char text[64];
    std::snprintf(text, sizeof(text), "Scan: %d/%d (Esc cancels)", captured_, frameCount_);
    return text;
}

void GrayCodeScan::patternsReplied(std::optional<int> frameCount, const std::string& failStatus) {
    if (!awaitingPatterns()) return;
    if (!frameCount) {  // the request failed outright
        fail(failStatus.empty() ? "Scan: the AI sidecar could not write the patterns" : failStatus);
        return;
    }
    if (*frameCount < 3) {  // at least one pattern, then white and black
        fail(failStatus.empty() ? "Scan: the AI sidecar wrote no patterns" : failStatus);
        return;
    }
    frameCount_ = *frameCount;
}

void GrayCodeScan::fail(const std::string& status) {
    if (isOver()) return;
    restore();
    endStatus_ = status;
    phase_ = Phase::Ended;
}

void GrayCodeScan::restore() {
    if (frameShown_) host_.hideFrames();
    frameShown_ = false;
    // Only what the scan changed, and only if it still stands: an Esc that already left
    // perform mode is the user's.
    if (enteredPerform_ && host_.inPerformMode()) host_.exitPerformMode();
    enteredPerform_ = false;
    camera_.reset();  // closes the camera
}

std::string GrayCodeScan::interruption() const {
    if (host_.locked()) return "Scan cancelled: the show was locked";
    if (host_.blackout()) return "Scan cancelled: Blackout is on";
    const std::optional<ScanOutput> now = host_.output(outputId_);
    if (!now) return "Scan cancelled: the output was removed";
    // The display and its mode are fixed once the frames were asked for at its size.
    if (patternsRequested_ && *now != output_) {
        return "Scan cancelled: the output's resolution or display changed";
    }
    // Once in perform mode, leaving it (Esc or F11 on a projector window) cancels.
    if (performSeen_ && !host_.inPerformMode()) return "Scan cancelled";
    return {};
}

void GrayCodeScan::requestPatterns() {
    // Black while the output goes fullscreen, the camera opens and the frames are written.
    host_.showBlack();
    frameShown_ = true;
    if (!host_.inPerformMode()) {
        if (!host_.enterPerformMode()) {
            fail("Scan: the output could not go fullscreen (is its display connected?)");
            return;
        }
        enteredPerform_ = true;
    }
    performSeen_ = true;
    // Going fullscreen resolves the output's display afresh: the frames are made for
    // that display's size, and the scan, and its map, belong to it.
    const std::optional<ScanOutput> settled = host_.output(outputId_);
    if (!settled) {
        fail("Scan cancelled: the output was removed");
        return;
    }
    if (settled->monitor < 0) {
        fail("Scan: the output has no display - pick one for it in Outputs");
        return;
    }
    output_ = *settled;
    patternSize_ = output_.resolution;
    if (!host_.requestPatterns(patternSize_)) {
        fail("Scan: could not ask the AI sidecar for the patterns");
        return;
    }
    patternsRequested_ = true;
}

void GrayCodeScan::show(int index, Clock::time_point now) {
    index_ = index;
    ticksShown_ = 0;
    shownAt_ = now;
    if (!host_.showFrame(index)) {
        fail("Scan: could not load " + patternName(index));
        return;
    }
    frameShown_ = true;
    phase_ = Phase::Settling;
}

void GrayCodeScan::tick(Clock::time_point now) {
    if (isOver()) return;
    const std::string why = interruption();
    if (!why.empty()) {
        fail(why);
        return;
    }
    if (camera_ && camera_->state() == ScanCamera::State::Failed) {
        fail("Scan: " + camera_->error());
        return;
    }

    switch (phase_) {
        case Phase::Preparing:
            if (!patternsRequested_) {
                requestPatterns();  // first tick: fullscreen, then the frames at its size
                return;
            }
            if (frameCount_ == 0 || !camera_ || camera_->state() != ScanCamera::State::Ready) return;
            show(0, now);
            return;

        case Phase::Settling: {
            ++ticksShown_;
            const double settle = settleSeconds_ + (index_ == 0 ? FIRST_FRAME_EXTRA_SECONDS : 0.0);
            const double elapsed = std::chrono::duration<double>(now - shownAt_).count();
            if (ticksShown_ < MIN_TICKS_SHOWN || elapsed < settle) return;
            camera_->capture(utf8FromPath(folder_ / captureName(index_)));
            phase_ = Phase::Capturing;
            return;
        }

        case Phase::Capturing:
            if (camera_->state() != ScanCamera::State::Ready) return;
            captured_ = index_ + 1;
            if (captured_ < frameCount_) {
                show(captured_, now);
                return;
            }
            restore();
            phase_ = Phase::Captured;
            return;

        case Phase::Captured:
        case Phase::Ended:
            return;
    }
}

}  // namespace quadmapper
