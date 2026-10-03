#include "AiAssist.hpp"
#include "../editor/EditorState.hpp"
#include "../editor/PerformLock.hpp"
#include "../ai/SidecarClient.hpp"
#include "../core/FileSystem.hpp"
#include "../core/Logger.hpp"
#include "../output/OutputManager.hpp"
#include "../renderer/Renderer.hpp"
#include "../renderer/Texture.hpp"
#include "../scene/Quad.hpp"
#include "../source/CameraDevices.hpp"
#include "../source/CameraSource.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>

namespace quadmapper {

namespace {

void showStatus(EditorState& es, const std::string& msg, float seconds) {
    snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
    es.statusTimer = seconds;
}

std::string formatStatus(const char* fmt, ...) {
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    return msg;
}

// The busy status stays up until the outcome replaces it: longer than the camera
// pump plus the sidecar's 30 s request ceiling.
constexpr float BUSY_STATUS_SECONDS = 60.0f;
// Shutdown waits this long for a running request, then lets its thread go: it holds
// only its own shares (Request, the sidecar) and never touches AiAssist or the editor.
constexpr auto SHUTDOWN_WAIT = std::chrono::seconds(2);
constexpr float CONFIDENCE_THRESHOLD = 0.35f;

}  // namespace

// A request in flight. The worker fills `outcome` and sets `done`; tick() collects
// it on the main thread. `who` and `target` are read on the main thread only.
struct AiAssist::Request {
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    Outcome outcome;
    std::string who;
    UUID target;
};

AiAssist::AiAssist(EditorState& editor, std::unique_ptr<ai::SidecarClient> sidecar)
    : editor_(editor),
      sidecar_(std::move(sidecar)),
      lastSidecarState_(ai::SidecarState::Stopped),
      selfTestPending_(std::getenv("QM_AI_SELFTEST") != nullptr) {}

AiAssist::~AiAssist() {
    if (!requestThread_.joinable()) return;
    // A launched sidecar dies here (no orphan), which also ends a running HTTP call
    // at once instead of after the wait below. Idempotent after stopSidecar().
    if (sidecar_) sidecar_->stop();
    bool done;
    {
        std::unique_lock<std::mutex> lock(request_->mutex);
        done = request_->finished.wait_for(lock, SHUTDOWN_WAIT,
                                           [this] { return request_->done; });
    }
    if (done) {
        requestThread_.join();
    } else {
        // Stuck in the camera or an HTTP call to a sidecar we do not own: never hold
        // the exit hostage to its 30 s timeout.
        Logger::warning("AI: a request was still running at shutdown; leaving it to finish");
        requestThread_.detach();
    }
}

ai::SidecarConfig AiAssist::locateSidecar() {
    ai::SidecarConfig config;
    const std::filesystem::path rel("tools/ai-sidecar/sidecar.py");
    for (const std::filesystem::path& base :
         {std::filesystem::current_path(),
          std::filesystem::current_path() / ".." / "..",
          std::filesystem::current_path() / ".." / ".." / ".."}) {
        std::error_code ec;
        std::filesystem::path candidate = base / rel;
        if (std::filesystem::exists(candidate, ec)) {
            config.scriptPath = utf8FromPath(std::filesystem::weakly_canonical(candidate, ec));
            break;
        }
    }
    return config;
}

void AiAssist::setStatus(const std::string& msg, float seconds) {
    showStatus(editor_, msg, seconds);
    Logger::info(msg);
}

bool AiAssist::sidecarReady() const {
    return sidecar_ &&
           (sidecar_->state() == ai::SidecarState::Attached ||
            sidecar_->state() == ai::SidecarState::Launched);
}

bool AiAssist::isActive() const {
    return sidecarReady();
}

bool AiAssist::isBusy() const {
    return request_ != nullptr;
}

bool AiAssist::startRequest(const std::string& busyStatus, const std::string& who,
                            const UUID& target, std::function<Outcome()> work) {
    if (request_) {
        setStatus(who + ": wait for the running request to finish", 5.0f);
        return false;
    }
    auto request = std::make_shared<Request>();
    request->who = who;
    request->target = target;
    requestThread_ = std::thread([request, who, work = std::move(work)]() mutable {
        Outcome outcome;
        try {
            outcome = work();
        } catch (const std::exception& e) {
            Logger::error("{}: the request failed: {}", who, e.what());
            outcome = Outcome{who + ": the request failed", 6.0f, std::nullopt};
        } catch (...) {
            Logger::error("{}: the request failed", who);
            outcome = Outcome{who + ": the request failed", 6.0f, std::nullopt};
        }
        work = nullptr;  // what it captured (a sidecar share) is freed here, before done
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->outcome = std::move(outcome);
            request->done = true;
        }
        request->finished.notify_all();
    });
    request_ = std::move(request);
    setStatus(busyStatus, BUSY_STATUS_SECONDS);
    return true;
}

void AiAssist::applyOutcome(const Request& request) {
    const Outcome& outcome = request.outcome;
    if (outcome.corners) {
        // The lock may have engaged while the request ran: it never lets a warp through.
        if (editor_.performLock.isOn()) {
            setStatus(request.who +
                          ": result dropped - the show is locked (Output > Perform Lock)",
                      6.0f);
            return;
        }
        Quad* quad = editor_.quadManager().getQuad(request.target);
        if (!quad) {
            setStatus(request.who + ": result dropped - the quad was deleted", 6.0f);
            return;
        }
        // TL,TR,BR,BL, normalized [0,1]: the same order and space as QuadGeometry.
        // The result is an ordinary warp: drawn on canvas, F11-projectable, and
        // refinable with Tab-cycle + arrow-nudge.
        const auto& c = *outcome.corners;
        quad->getGeometry().setCorners({Vec2{c[0][0], c[0][1]}, Vec2{c[1][0], c[1][1]},
                                        Vec2{c[2][0], c[2][1]}, Vec2{c[3][0], c[3][1]}});
    }
    setStatus(outcome.status, outcome.seconds);
}

void AiAssist::enable() {
    if (!sidecar_) return;
    sidecar_->startAsync();
    showStatus(editor_, "AI: connecting to sidecar...", 5.0f);
}

void AiAssist::run(EditorServices::AiAction action, OutputManager* outputs) {
    using AiAction = EditorServices::AiAction;
    if (action != AiAction::ClearPattern && refusedByPerformLock(editor_, "AI assist")) return;
    switch (action) {
        case AiAction::Enable:        enable(); break;
        case AiAction::DetectSurface: detectSurface(); break;
        case AiAction::EmitPattern:   if (outputs) emitPattern(*outputs); break;
        case AiAction::CapturePhoto:  capturePhoto(outputs); break;
        case AiAction::ClearPattern:  clearPattern(); break;
        case AiAction::SolveApply:    solveAndApply(outputs); break;
        case AiAction::Anamorphic:    anamorphic(outputs); break;
    }
}

void AiAssist::tick(OutputManager* outputs) {
    // TD-10: when the cached state leaves Starting, post a one-shot status so the
    // user sees connected/failed without a freeze.
    if (sidecar_) {
        const ai::SidecarState s = sidecar_->state();
        if (s != lastSidecarState_) {
            if (lastSidecarState_ == ai::SidecarState::Starting) {
                const bool ok = (s == ai::SidecarState::Attached ||
                                 s == ai::SidecarState::Launched);
                showStatus(editor_,
                           ok ? "AI assist enabled (sidecar connected)"
                              : "AI assist unavailable - sidecar could not start",
                           5.0f);
                if (ok) Logger::info(editor_.statusMessage);
                else    Logger::warning(editor_.statusMessage);
            }
            lastSidecarState_ = s;
        }
    }

    if (request_) {
        bool done;
        {
            std::lock_guard<std::mutex> lock(request_->mutex);
            done = request_->done;
        }
        if (done) {
            requestThread_.join();  // the work is over; this only waits out its exit
            const std::shared_ptr<Request> request = std::move(request_);
            applyOutcome(*request);
        }
    }

    // QM-058 verification hook (off unless QM_AI_SELFTEST is set): once the
    // first frames have built the default scene, enable the sidecar, select a
    // quad, and run one AI action headlessly so the full chain can be asserted
    // from the log without driving ImGui menus. No-op in production.
    if (selfTestPending_ && ++frameCount_ >= 3) {
        selfTestPending_ = false;
        runSelfTest(outputs);
    }
}

void AiAssist::runSelfTest(OutputManager* outputs) {
    EditorState& es = editor_;
    if (!es.quadManager().getPrimarySelectedQuad() && !es.quadManager().getQuads().empty()) {
        es.quadManager().selectQuad(es.quadManager().getQuads().front()->getId());
    }
    if (sidecar_) {
        sidecar_->start();
    }

    const char* modeEnv = std::getenv("QM_AI_SELFTEST");
    const std::string mode = modeEnv ? modeEnv : "";
    if (mode == "oneflash") {
        // QM-059: arm the pattern, flash it on the projector, capture a photo.
        emitPattern(*outputs);
        outputs->handleF11Key();   // enter perform mode -> pattern shows
        capturePhoto(outputs);
    } else if (mode == "solve") {
        // QM-060: solve the projector->surface homography and apply the warp.
        solveAndApply(outputs);
    } else if (mode == "anamorphic") {
        // QM-062: audience-perspective (frontal-from-viewpoint) warp.
        anamorphic(outputs);
    } else {
        detectSurface();
    }
}

void AiAssist::detectSurface() {
    // Preconditions: a running sidecar and a target quad to warp.
    if (!sidecarReady()) {
        setStatus("AI: enable the sidecar first (AI > Enable AI Assist)", 5.0f);
        return;
    }
    Quad* quad = editor_.quadManager().getPrimarySelectedQuad();
    if (!quad) {
        setStatus("AI: select a quad to warp first", 5.0f);
        return;
    }

    // The input image: QM_AI_TEST_IMAGE feeds a fixed photo (verification, and the
    // "detect from a phone photo on disk" path); otherwise the worker captures a live
    // frame off the first camera (the QM-055 rail). Camera, capture and inference run
    // off the main thread, so the editor and the projectors keep rendering.
    std::string testImage;
    if (const char* testImg = std::getenv("QM_AI_TEST_IMAGE")) {
        const std::filesystem::path path(testImg);
        if (!std::filesystem::exists(path)) {
            setStatus("AI: QM_AI_TEST_IMAGE not found", 5.0f);
            return;
        }
        testImage = utf8FromPath(path);
    }

    std::shared_ptr<ai::SidecarClient> sidecar = sidecar_;
    startRequest("AI: detecting surface...", "AI", quad->getId(),
                 [sidecar, testImage]() -> Outcome {
        std::string imgPath = testImage;
        if (imgPath.empty()) {
            imgPath = utf8FromPath(std::filesystem::temp_directory_path() /
                                   "quadmapper_ai_capture.png");
            std::string err;
            if (!captureCameraFrameToPng(imgPath, err)) {
                return Outcome{"AI: " + err, 5.0f, std::nullopt};
            }
        }

        auto surface = sidecar->requestSurface(imgPath);
        if (!surface) {
            return Outcome{"AI: detection failed (sidecar error)", 5.0f, std::nullopt};
        }

        // Low confidence: do NOT auto-warp onto a possibly-wrong surface — ask the
        // user to disambiguate instead (the spike's load-bearing human-confirm step).
        if (surface->confidence < CONFIDENCE_THRESHOLD) {
            return Outcome{formatStatus("AI: low confidence (%.2f) - tap the surface to retry",
                                        surface->confidence),
                           5.0f, std::nullopt};
        }
        return Outcome{formatStatus("AI: surface applied (confidence %.2f) - nudge with Tab + arrows",
                                    surface->confidence),
                       5.0f, surface->corners};
    });
}

bool AiAssist::captureCameraFrameToPng(const std::string& pngPath, std::string& error) {
    const auto cameras = enumerateCameraDevices();
    if (cameras.empty()) {
        error = "no camera found (connect one or set QM_AI_TEST_IMAGE)";
        return false;
    }
    CameraSource camera(cameras.front().name);
    if (!camera.initialize()) {
        error = "failed to open camera";
        return false;
    }
    // Pump updates until the first frame lands (or give up after ~1s).
    for (int i = 0; i < 60 && !camera.hasNewFrame(); ++i) {
        camera.update(0.016);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const bool captured = camera.captureCurrentFrameToPng(pathFromUtf8(pngPath));
    camera.shutdown();
    if (!captured) {
        error = "camera capture failed (no frame)";
        return false;
    }
    return true;
}

void AiAssist::emitPattern(const OutputManager& outputs) {
    // Lazily load the calibration pattern + build the fullscreen identity quad.
    if (!calibTexture_) {
        std::filesystem::path patternPath;
        const std::filesystem::path rel("resources/patterns/charuco.png");
        for (const std::filesystem::path& base :
             {std::filesystem::current_path(),
              std::filesystem::current_path() / "..",
              std::filesystem::current_path() / ".." / ".."}) {
            std::error_code ec;
            if (std::filesystem::exists(base / rel, ec)) {
                patternPath = base / rel;
                break;
            }
        }
        auto tex = std::make_unique<Texture>();
        if (patternPath.empty() || !tex->loadFromFile(patternPath)) {
            setStatus("One-Flash: pattern asset not found (resources/patterns/charuco.png)", 5.0f);
            return;
        }
        calibTexture_ = std::move(tex);
        calibQuad_ = std::make_unique<Quad>();
        calibQuad_->getGeometry().setCorners(
            {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)});
    }

    emitPattern_ = true;
    if (!outputs.isInPerformMode()) {
        setStatus("One-Flash: pattern armed - press F11 to flash it on the projector", 5.0f);
    } else {
        setStatus("One-Flash: pattern on projector - capture a photo, then Clear Pattern", 5.0f);
    }
}

void AiAssist::capturePhoto(const OutputManager* outputs) {
    if (outputs && outputs->isBlackout()) {  // the photo would be black
        setStatus("One-Flash: Blackout is on - turn it off to capture", 6.0f);
        return;
    }
    const std::string out = utf8FromPath(std::filesystem::temp_directory_path() /
                                         "quadmapper_oneflash_capture.png");
    startRequest("One-Flash: capturing photo...", "One-Flash", UUID::null(),
                 [out]() -> Outcome {
        std::string err;
        if (!captureCameraFrameToPng(out, err)) {
            return Outcome{"One-Flash: " + err, 6.0f, std::nullopt};
        }
        return Outcome{"One-Flash: captured " + out, 6.0f, std::nullopt};
    });
}

void AiAssist::clearPattern() {
    emitPattern_ = false;
    setStatus("One-Flash: pattern cleared", 4.0f);
}

void AiAssist::solveAndApply(const OutputManager* outputs) {
    runOneFlash(outputs, nullptr, "warp applied");
}

void AiAssist::anamorphic(const OutputManager* outputs) {
    // A centered rectangle in the audience (photo) view: content mapped here
    // looks like a frontal rectangle from the captured viewpoint.
    static const std::array<std::array<float, 2>, 4> kAudienceTarget{{
        {0.15f, 0.15f}, {0.85f, 0.15f}, {0.85f, 0.85f}, {0.15f, 0.85f}}};
    runOneFlash(outputs, &kAudienceTarget, "anamorphic warp applied");
}

void AiAssist::runOneFlash(const OutputManager* outputs,
                           const std::array<std::array<float, 2>, 4>* audienceTarget,
                           const char* label) {
    // Under blackout the projectors show no pattern, so the camera would photograph
    // black and the solve would report a detection failure.
    if (outputs && outputs->isBlackout()) {
        setStatus("One-Flash: Blackout is on - turn it off to capture", 6.0f);
        return;
    }
    if (!sidecarReady()) {
        setStatus("One-Flash: enable the sidecar first (AI > Enable AI Assist)", 6.0f);
        return;
    }
    Quad* quad = editor_.quadManager().getPrimarySelectedQuad();
    if (!quad) {
        setStatus("One-Flash: select a quad to warp first", 6.0f);
        return;
    }

    // The pattern photo: QM_AI_TEST_IMAGE override (a synthetic capture for
    // verification) else a live camera shot of the projected pattern, taken by the
    // worker along with the solve, off the main thread.
    std::string testImage;
    if (const char* testImg = std::getenv("QM_AI_TEST_IMAGE")) {
        const std::filesystem::path path(testImg);
        if (!std::filesystem::exists(path)) {
            setStatus("One-Flash: QM_AI_TEST_IMAGE not found", 6.0f);
            return;
        }
        testImage = utf8FromPath(path);
    }

    std::optional<std::array<std::array<float, 2>, 4>> target;
    if (audienceTarget) target = *audienceTarget;
    std::shared_ptr<ai::SidecarClient> sidecar = sidecar_;
    startRequest("One-Flash: solving...", "One-Flash", quad->getId(),
                 [sidecar, testImage, target, label = std::string(label)]() -> Outcome {
        std::string patternPath = testImage;
        if (patternPath.empty()) {
            patternPath = utf8FromPath(std::filesystem::temp_directory_path() /
                                       "quadmapper_oneflash_capture.png");
            std::string err;
            if (!captureCameraFrameToPng(patternPath, err)) {
                return Outcome{"One-Flash: " + err, 6.0f, std::nullopt};
            }
        }

        auto result = sidecar->requestOneFlash(patternPath, target ? &*target : nullptr);
        if (!result || result->stub) {
            return Outcome{"One-Flash: pattern not detected (need the ChArUco visible)", 6.0f,
                           std::nullopt};
        }
        if (result->confidence < CONFIDENCE_THRESHOLD) {
            return Outcome{formatStatus("One-Flash: low confidence (%.2f) - reframe the pattern",
                                        result->confidence),
                           6.0f, std::nullopt};
        }
        return Outcome{formatStatus("One-Flash: %s (confidence %.2f) - send to projector (F11)",
                                    label.c_str(), result->confidence),
                       6.0f, result->corners};
    });
}

bool AiAssist::renderPatternFrames(OutputManager& outputs, Renderer& renderer) {
    if (!emitPattern_ || !calibQuad_) return false;
    outputs.renderPatternFrames(*calibQuad_, calibTexture_.get(), renderer);
    return true;
}

void AiAssist::stopSidecar() {
    if (!sidecar_) return;
    // Kill any launched sidecar now (no orphan), even while a running request still
    // holds a share of the client: its HTTP call then fails fast instead of waiting
    // out the 30 s timeout.
    sidecar_->stop();
    sidecar_.reset();
}

} // namespace quadmapper
