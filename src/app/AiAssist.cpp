#include "AiAssist.hpp"
#include "../editor/EditorState.hpp"
#include "../editor/PerformLock.hpp"
#include "../ai/SidecarClient.hpp"
#include "../core/FileSystem.hpp"
#include "../core/Logger.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../renderer/Renderer.hpp"
#include "../renderer/Texture.hpp"
#include "../scene/CalibrationSession.hpp"
#include "../scene/Commands.hpp"
#include "../scene/Quad.hpp"
#include "../source/CameraDevices.hpp"
#include "../source/CameraSource.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
// pump plus the sidecar's request ceiling (SidecarConfig::requestTimeoutMs, 120 s).
constexpr float BUSY_STATUS_SECONDS = 150.0f;
// Shutdown waits this long for a running request, then lets its thread go: it holds
// only its own shares (Request, the sidecar) and never touches AiAssist or the editor.
constexpr auto SHUTDOWN_WAIT = std::chrono::seconds(2);
constexpr const char* PICK = "Pick Surface";
constexpr const char* SCAN = "Scan";
constexpr const char* CALIBRATION = "Calibration";
// A decoded map that saw less of the output than this is refused: the camera was not
// aimed at it.
constexpr float MIN_SCAN_COVERAGE = 0.02f;
// Corners from the AI engine (normalized canvas units) outside this range, or not
// finite, are refused before they reach a quad: /oneflash projects through a homography
// and does not clamp.
constexpr float CORNER_MIN = -4.0f;
constexpr float CORNER_MAX = 5.0f;

bool cornersInRange(const std::array<std::array<float, 2>, 4>& corners) {
    for (const auto& corner : corners) {
        for (float v : corner) {
            if (!std::isfinite(v) || v < CORNER_MIN || v > CORNER_MAX) return false;
        }
    }
    return true;
}

// The scan's folder for one output: the sidecar writes the frames there and the camera's
// captures and the map land beside them.
std::filesystem::path scanFolder(const UUID& outputId) {
    return std::filesystem::temp_directory_path() / "quadmapper_scan" / outputId.toString();
}

AiAssist::Outcome scanPatternsOutcome(const ai::GrayReply<ai::GrayPatternsResult>& reply) {
    AiAssist::Outcome outcome;
    outcome.seconds = 6.0f;
    outcome.scanFrames = 0;
    if (reply.endpointMissing) {
        outcome.status = "Scan: the AI features are out of date: update or reinstall them";
    } else if (!reply.error.empty()) {
        outcome.status = "Scan: " + reply.error;
    } else if (!reply.result) {
        outcome.status = "Scan: the AI engine could not write the patterns";
    } else if (reply.result->stub) {
        outcome.status = "Scan: the AI features are out of date: update or reinstall them";
    } else {
        outcome.scanFrames = reply.result->count;
    }
    return outcome;
}

AiAssist::Outcome scanDecodeOutcome(const ai::GrayReply<ai::GrayCodeResult>& reply,
                                    const UUID& outputId, const ScanOutput& output) {
    AiAssist::Outcome outcome;
    outcome.seconds = 8.0f;
    if (reply.endpointMissing) {
        outcome.status = "Scan: the AI features are out of date: update or reinstall them";
    } else if (!reply.error.empty()) {
        outcome.status = "Scan: decoding failed - " + reply.error;
    } else if (!reply.result || reply.result->stub) {
        outcome.status = "Scan: decoding failed (AI engine error)";
    } else if (reply.result->coverage < MIN_SCAN_COVERAGE) {
        outcome.status = "Scan: the camera saw almost none of the output - aim it at the output and scan again";
    } else {
        const ai::GrayCodeResult& r = *reply.result;
        outcome.status = formatStatus("Scan: map ready - the camera saw %.0f%% of the output (noise %.2f px)",
                                      r.coverage * 100.0f, r.decodeErrorPx.value_or(0.0f));
        outcome.grayMap = AiAssist::GrayMap{outputId, output, r.mapPath, r.coverage, r.decodeErrorPx};
    }
    return outcome;
}

}  // namespace

// The scan's view of the application: the editor's lock, the outputs, and the frame
// texture the projectors show (renderPatternFrames).
class AiAssist::ScanHostImpl : public ScanHost {
public:
    ScanHostImpl(AiAssist& ai, OutputManager& outputs, std::filesystem::path folder)
        : ai_(ai), outputs_(outputs), folder_(std::move(folder)) {}

    bool locked() const override { return ai_.editor_.performLock.isOn(); }
    bool blackout() const override { return outputs_.isBlackout(); }
    bool inPerformMode() const override { return outputs_.isInPerformMode(); }
    bool enterPerformMode() override {
        outputs_.handleF11Key();  // as F11 does, with the user's fullscreen kind
        return outputs_.isInPerformMode();
    }
    void exitPerformMode() override { outputs_.exitPerformMode(); }
    std::optional<ScanOutput> output(const UUID& outputId) const override {
        const OutputWindow* output = outputs_.getOutput(outputId);
        if (!output) return std::nullopt;
        return AiAssist::performedOutput(*output, outputs_.getMonitors());
    }
    bool requestPatterns(IVec2 size) override {
        std::shared_ptr<ai::SidecarClient> sidecar = ai_.sidecar_;
        if (!sidecar) return false;
        return ai_.startRequest("Scan: preparing the patterns...", SCAN, UUID::null(),
                                [sidecar, folder = utf8FromPath(folder_), size]() -> Outcome {
            return scanPatternsOutcome(sidecar->requestGrayPatterns(folder, size.x, size.y));
        });
    }
    void showBlack() override {
        ai_.scanTexture_.reset();
        ai_.scanFrameShown_ = true;
    }
    bool showFrame(int index) override {
        auto texture = std::make_unique<Texture>();
        if (!texture->loadFromFile(folder_ / GrayCodeScan::patternName(index))) return false;
        texture->setFilterMode(false);  // stripes stay sharp if the frame is resampled
        ai_.scanTexture_ = std::move(texture);
        ai_.scanFrameShown_ = true;
        return true;
    }
    void hideFrames() override {
        ai_.scanFrameShown_ = false;
        ai_.scanTexture_.reset();
    }

private:
    AiAssist& ai_;
    OutputManager& outputs_;
    std::filesystem::path folder_;
};

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
        // the exit hostage to its request timeout.
        Logger::warning("AI: a request was still running at shutdown; leaving it to finish");
        requestThread_.detach();
    }
}

ai::SidecarConfig AiAssist::locateSidecar() {
    return locateSidecarFrom(executableDir());
}

ai::SidecarConfig AiAssist::locateSidecarFrom(const std::filesystem::path& exeDir) {
    namespace fs = std::filesystem;
    ai::SidecarConfig config;
    if (exeDir.empty() || !exeDir.is_absolute()) return config;  // never the current folder
    std::error_code ec;

    // The installed layout (ADR-057 §4): the bundled interpreter, script and models.
    const fs::path installed = exeDir / "ai";
    if (fs::is_regular_file(installed / "python" / "python.exe", ec) &&
        fs::is_regular_file(installed / "sidecar" / "sidecar.py", ec)) {
        config.pythonExe = utf8FromPath(installed / "python" / "python.exe");
        config.scriptPath = utf8FromPath(installed / "sidecar" / "sidecar.py");
        if (fs::is_directory(installed / "models", ec)) {
            config.modelsDir = utf8FromPath(installed / "models");
        }
        return config;
    }

    // A developer build: the repository's sidecar with its own environment (setup.ps1),
    // in a folder above the executable.
    fs::path dir = exeDir.lexically_normal();
    if (!dir.has_filename() && dir.has_relative_path()) dir = dir.parent_path();  // "C:\a\b\"
    for (int level = 0; level <= 6; ++level) {
        const fs::path sidecar = dir / "tools" / "ai-sidecar";
        const fs::path python = sidecar / ".venv" / "Scripts" / "python.exe";
        if (fs::is_regular_file(sidecar / "sidecar.py", ec) && fs::is_regular_file(python, ec)) {
            config.pythonExe = utf8FromPath(python);
            config.scriptPath = utf8FromPath(sidecar / "sidecar.py");
            return config;
        }
        if (!dir.has_relative_path()) break;  // at the root
        dir = dir.parent_path();
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
    return request_ != nullptr || scan_ != nullptr;
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

void AiAssist::applyOutcome(const Request& request, OutputManager* outputs) {
    const Outcome& outcome = request.outcome;
    // While a scan waits for its frames, the request that just ended is that one: its
    // reply, or its failure (no frame count, as when it threw), ends the wait.
    if (scan_ && scan_->awaitingPatterns()) {
        scan_->patternsReplied(outcome.scanFrames, outcome.status);
        return;
    }
    if (outcome.scanFrames) return;  // the frames of a scan cancelled meanwhile
    if (outcome.corners && !cornersInRange(*outcome.corners)) {
        setStatus("AI: result ignored (corners out of range)", 6.0f);
        return;
    }
    if (outcome.grayMap) grayMaps_[outcome.grayMap->outputId] = *outcome.grayMap;
    if (outcome.photo) {
        photo_.path = *outcome.photo;
        ++photo_.serial;
        photo_.proposal.reset();  // it was drawn on the previous photo
    }
    if (outcome.oneFlashPhoto) oneFlashPhoto_ = *outcome.oneFlashPhoto;
    if (outcome.forgetOneFlashPhoto) oneFlashPhoto_.clear();
    // The lock may have engaged while the request ran: it never lets a warp through,
    // nor a proposal that would invite one.
    if ((outcome.corners || outcome.proposal || outcome.calibrationPixel) &&
        editor_.performLock.isOn()) {
        setStatus(request.who + ": result dropped - the show is locked (Output > Perform Lock)",
                  6.0f);
        return;
    }
    if (outcome.proposal) photo_.proposal = *outcome.proposal;
    if (outcome.calibrationPixel && !applyCalibrationPixel(*outcome.calibrationPixel, outputs)) {
        return;
    }
    if (outcome.corners) {
        Quad* quad = editor_.quadManager().getQuad(request.target);
        if (!quad) {
            setStatus(request.who + ": result dropped - the quad was deleted", 6.0f);
            return;
        }
        // TL,TR,BR,BL, normalized [0,1]: the same order and space as QuadGeometry.
        // The result is an ordinary warp: drawn on canvas, F11-projectable, and
        // refinable with Tab-cycle + arrow-nudge, and one undo step.
        const auto& c = *outcome.corners;
        const std::array<Vec2, 4> after{Vec2{c[0][0], c[0][1]}, Vec2{c[1][0], c[1][1]},
                                        Vec2{c[2][0], c[2][1]}, Vec2{c[3][0], c[3][1]}};
        editor_.commandHistory.execute(std::make_unique<QuadPropertyCommand<std::array<Vec2, 4>>>(
            "AI Warp", editor_.quadRef(*quad), quad->getGeometry().getCorners(), after,
            [](Quad& q, const std::array<Vec2, 4>& corners) { q.getGeometry().setCorners(corners); }));
    }
    setStatus(outcome.status, outcome.seconds);
}

void AiAssist::enable() {
    if (!sidecar_) return;
    sidecar_->startAsync();
    showStatus(editor_, "AI: starting the AI engine...", 5.0f);
}

void AiAssist::run(EditorServices::AiAction action, OutputManager* outputs) {
    using AiAction = EditorServices::AiAction;
    // Clear Pattern and Cancel Scan give the show back, so the lock lets them through.
    if (action != AiAction::ClearPattern && action != AiAction::CancelScan &&
        refusedByPerformLock(editor_, "AI assist")) {
        return;
    }
    switch (action) {
        case AiAction::Enable:        enable(); break;
        case AiAction::DetectSurface: detectSurface(outputs); break;
        case AiAction::EmitPattern:   if (outputs) emitPattern(*outputs); break;
        case AiAction::CapturePhoto:  capturePhoto(outputs); break;
        case AiAction::ClearPattern:  clearPattern(); break;
        case AiAction::SolveApply:    solveAndApply(outputs); break;
        case AiAction::Anamorphic:    anamorphic(outputs); break;
        case AiAction::TakePhoto:     takePhoto(); break;
        case AiAction::ApplyProposal: applyProposal(outputs); break;
        case AiAction::ClearProposal: clearProposal(); break;
        case AiAction::ScanOutput:    scanOutput(outputs); break;
        case AiAction::CancelScan:    cancelScan(); break;
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
                std::string status = "AI Assist enabled (AI engine connected)";
                if (!ok) {
                    switch (sidecar_->failure()) {
                        case ai::SidecarFailure::NotInstalled:
                            status = "AI: the AI features are not installed";
                            break;
                        case ai::SidecarFailure::PortInUse:
                            status = formatStatus("AI: port %d is in use by another program",
                                                  sidecar_->port());
                            break;
                        default:
                            status = "AI Assist could not start: see the log file (Help > Open Log Folder)";
                            break;
                    }
                }
                showStatus(editor_, status, ok ? 5.0f : 8.0f);
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
            applyOutcome(*request, outputs);
        }
    }

    // Esc in the editor (performEscape) asks to cancel a running scan.
    if (scan_ && editor_.aiScan.cancelRequested) scan_->cancel();
    editor_.aiScan.cancelRequested = false;
    if (scan_) {
        scan_->tick(GrayCodeScan::Clock::now());
        if (scan_->phase() == GrayCodeScan::Phase::Captured) {
            startScanDecode();
        } else if (scan_->phase() == GrayCodeScan::Phase::Ended) {
            const std::string status = scan_->endStatus();
            scan_.reset();
            scanHost_.reset();
            scanProgress_.clear();
            setStatus(status, 6.0f);
        } else if (!request_ && scan_->progress() != scanProgress_) {
            scanProgress_ = scan_->progress();
            showStatus(editor_, scanProgress_, BUSY_STATUS_SECONDS);  // each frame: not logged
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
    editor_.aiScan.running = scan_ != nullptr;
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
        detectSurface(outputs);
    }
}

void AiAssist::detectSurface(const OutputManager* outputs) {
    // Preconditions: a running sidecar and a target quad to warp.
    if (!sidecarReady()) {
        setStatus("AI: turn on AI Assist first (AI > Enable AI Assist)", 5.0f);
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

    // Its corners map into projector space as Pick Surface's do (ADR-053 §3).
    GrayMapFn grayMap;
    OneFlashFn oneFlash;
    photoMappings(outputs, grayMap, oneFlash);
    std::shared_ptr<ai::SidecarClient> sidecar = sidecar_;
    startRequest("AI: detecting surface...", "AI", quad->getId(),
                 [sidecar, testImage, grayMap, oneFlash]() -> Outcome {
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
            return Outcome{"AI: detection failed (AI engine error)", 5.0f, std::nullopt};
        }

        // Low confidence: do NOT auto-warp onto a possibly-wrong surface — ask the
        // user to disambiguate instead (the spike's load-bearing human-confirm step).
        if (surface->confidence < AI_MIN_CONFIDENCE) {
            return Outcome{formatStatus("AI: low confidence (%.2f) - tap the surface to retry",
                                        surface->confidence),
                           5.0f, std::nullopt};
        }
        return mapPhotoCorners(formatStatus("AI: surface found (confidence %.2f) - ", surface->confidence),
                               surface->corners, grayMap, oneFlash);
    });
}

bool AiAssist::captureCameraFrameToPng(const std::string& pngPath, std::string& error) {
    const auto cameras = enumerateCameraDevices();
    if (cameras.empty()) {
        error = "no camera found (connect one and try again)";
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

void AiAssist::ensurePatternQuad() {
    if (calibQuad_) return;
    calibQuad_ = std::make_unique<Quad>();
    calibQuad_->getGeometry().setCorners(
        {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)});
}

void AiAssist::emitPattern(const OutputManager& outputs) {
    // Lazily load the calibration pattern + build the fullscreen identity quad.
    if (!calibTexture_) {
        const std::filesystem::path patternPath = resourcePath("patterns/charuco.png");
        auto tex = std::make_unique<Texture>();
        std::error_code ec;
        if (!std::filesystem::exists(patternPath, ec) || !tex->loadFromFile(patternPath)) {
            Logger::warning("One-Flash: cannot load " + utf8FromPath(patternPath));
            setStatus("One-Flash: the calibration pattern file is missing: reinstall QUADmapper", 5.0f);
            return;
        }
        calibTexture_ = std::move(tex);
        ensurePatternQuad();
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
        Outcome outcome{"One-Flash: captured " + out, 6.0f, std::nullopt};
        outcome.oneFlashPhoto = out;
        return outcome;
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
        setStatus("One-Flash: turn on AI Assist first (AI > Enable AI Assist)", 6.0f);
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
        if (result->confidence < AI_MIN_CONFIDENCE) {
            return Outcome{formatStatus("One-Flash: low confidence (%.2f) - reframe the pattern",
                                        result->confidence),
                           6.0f, std::nullopt};
        }
        return Outcome{formatStatus("One-Flash: %s (confidence %.2f) - send to projector (F11)",
                                    label.c_str(), result->confidence),
                       6.0f, result->corners};
    });
}

// ---- Pick Surface (ADR-053 §2) ---------------------------------------------------------

bool AiAssist::canSegment() const {
    if (!sidecarReady()) return false;
    const std::vector<std::string> caps = sidecar_->capabilities();
    return std::find(caps.begin(), caps.end(), "segment") != caps.end();
}

void AiAssist::takePhoto() {
    // QM_AI_TEST_IMAGE stands in for the camera (verification, or a photo on disk).
    std::string testImage;
    if (const char* testImg = std::getenv("QM_AI_TEST_IMAGE")) {
        const std::filesystem::path path(testImg);
        if (!std::filesystem::exists(path)) {
            setStatus("AI Photo: QM_AI_TEST_IMAGE not found", 5.0f);
            return;
        }
        testImage = utf8FromPath(path);
    }
    startRequest("AI Photo: taking a photo...", "AI Photo", UUID::null(), [testImage]() -> Outcome {
        std::string path = testImage;
        if (path.empty()) {
            path = utf8FromPath(std::filesystem::temp_directory_path() / "quadmapper_ai_photo.png");
            std::string err;
            if (!captureCameraFrameToPng(path, err)) {
                return Outcome{"AI Photo: " + err, 5.0f, std::nullopt};
            }
        }
        Outcome outcome{"AI Photo: photo taken - click the surface to map", 5.0f, std::nullopt};
        outcome.photo = path;
        return outcome;
    });
}

void AiAssist::segmentAt(float x, float y) {
    if (refusedByPerformLock(editor_, "AI assist")) return;
    if (!sidecarReady()) {
        setStatus("Pick Surface: turn on AI Assist first (AI > Enable AI Assist)", 5.0f);
        return;
    }
    if (!canSegment()) {
        setStatus("Pick Surface: the AI features are out of date: update or reinstall them", 6.0f);
        return;
    }
    if (photo_.path.empty()) {
        setStatus("Pick Surface: take a photo first", 5.0f);
        return;
    }
    if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return;  // not on the photo

    std::shared_ptr<ai::SidecarClient> sidecar = sidecar_;
    const std::string path = photo_.path;
    const std::array<float, 2> point{x, y};
    startRequest("Pick Surface: finding the surface...", PICK, UUID::null(),
                 [sidecar, path, point]() -> Outcome {
        return segmentOutcome(sidecar->requestSegment(path, point));
    });
}

AiAssist::Outcome AiAssist::segmentOutcome(const ai::SegmentReply& reply) {
    if (reply.endpointMissing) {
        return Outcome{"Pick Surface: the AI features are out of date: update or reinstall them", 6.0f, std::nullopt};
    }
    if (!reply.result) {
        return Outcome{"Pick Surface: segmentation failed (AI engine error)", 5.0f, std::nullopt};
    }
    const ai::SegmentResult& r = *reply.result;
    AiSurfaceProposal proposal;
    proposal.corners = r.corners;
    proposal.outline = r.outline;
    proposal.confidence = r.confidence;
    proposal.planarity = r.planarity;
    proposal.stub = r.stub;

    Outcome outcome;
    outcome.seconds = 6.0f;
    if (proposal.stub) {
        outcome.status = "Pick Surface: the AI model files are missing: reinstall the AI features";
    } else if (!proposal.canApply()) {
        outcome.status = formatStatus("Pick Surface: low confidence (%.2f) - click elsewhere on the surface",
                                      proposal.confidence);
    } else {
        outcome.status = formatStatus("Pick Surface: surface found (confidence %.2f) - Apply to warp the selected quad",
                                      proposal.confidence);
    }
    outcome.proposal = std::move(proposal);
    return outcome;
}

AiAssist::PickMapping AiAssist::pickMapping(bool grayMapCurrent, const std::string& oneFlashPhoto) {
    if (grayMapCurrent) return PickMapping::GrayCode;
    return oneFlashPhoto.empty() ? PickMapping::CameraAtProjector : PickMapping::OneFlash;
}

AiAssist::Outcome AiAssist::mapPhotoCorners(const std::string& lead, const Corners& photoCorners,
                                            const GrayMapFn& grayMap, const OneFlashFn& oneFlash) {
    // Why a better mapping was passed over, for the status; empty when none was tried.
    std::string passedOver;
    if (grayMap) {
        const ai::GrayReply<ai::GrayMapResult> reply =
            grayMap(std::vector<std::array<float, 2>>(photoCorners.begin(), photoCorners.end()));
        if (reply.result &&
            std::all_of(reply.result->valid.begin(), reply.result->valid.end(), [](bool v) { return v; })) {
            Corners corners;
            std::copy(reply.result->points.begin(), reply.result->points.end(), corners.begin());
            return Outcome{lead + "applied through the scan - nudge with Tab + arrows", 6.0f, corners};
        }
        if (reply.endpointMissing) {
            passedOver = "update the AI features to map through the scan";
        } else if (reply.result) {
            passedOver = "a corner is outside the scanned area";
        } else if (!reply.error.empty()) {
            passedOver = "the scan could not map it (" + reply.error + ")";
        } else {
            passedOver = "the scan could not map it";
        }
    }
    const std::string because = passedOver.empty() ? lead : lead + passedOver + " - ";
    if (oneFlash) {
        // As the anamorphic path does: the photo corners go to /oneflash as
        // target_corners and come back in projector space.
        const std::optional<ai::SurfaceResult> result = oneFlash(photoCorners);
        if (result && !result->stub && result->confidence >= AI_MIN_CONFIDENCE) {
            return Outcome{because + formatStatus("applied through One-Flash (confidence %.2f) - "
                                                  "nudge with Tab + arrows",
                                                  result->confidence),
                           6.0f, result->corners};
        }
        // No usable pattern in that photo: never a dead end. Apply in photo space as
        // when there is no One-Flash photo, and forget this one.
        Outcome fallback{because + "the One-Flash photo shows no usable pattern - "
                                   "assuming the camera is at the projector",
                         6.0f, photoCorners};
        fallback.forgetOneFlashPhoto = true;
        return fallback;
    }
    return Outcome{passedOver.empty()
                       ? lead + "no One-Flash photo - assuming the camera is at the projector"
                       : because + "assuming the camera is at the projector",
                   6.0f, photoCorners};
}

AiAssist::PickMapping AiAssist::photoMappings(const OutputManager* outputs, GrayMapFn& grayMap,
                                              OneFlashFn& oneFlash) const {
    // The scan of an output showing the active scene (the quad's), when it is current.
    const GrayMap* gray = nullptr;
    if (outputs) {
        for (const auto& output : outputs->getOutputs()) {
            if (output->getSceneId() != editor_.activeScene().getId()) continue;
            if ((gray = currentGrayMap(output->getId(), outputs)) != nullptr) break;
        }
    }
    std::shared_ptr<ai::SidecarClient> sidecar = sidecar_;
    grayMap = nullptr;
    oneFlash = nullptr;
    if (gray && sidecar) {
        grayMap = [sidecar, path = gray->path](const std::vector<std::array<float, 2>>& points) {
            return sidecar->requestGrayMap(path, points);
        };
    }
    if (!oneFlashPhoto_.empty() && sidecar) {
        oneFlash = [sidecar, pattern = oneFlashPhoto_](const Corners& target) {
            return sidecar->requestOneFlash(pattern, &target);
        };
    }
    return pickMapping(gray != nullptr, oneFlashPhoto_);
}

void AiAssist::applyProposal(const OutputManager* outputs) {
    if (!photo_.proposal) {
        setStatus("Pick Surface: click the surface in the photo first", 5.0f);
        return;
    }
    if (!photo_.proposal->canApply()) {
        setStatus("Pick Surface: the proposal is not confident enough to apply", 5.0f);
        return;
    }
    Quad* quad = editor_.quadManager().getPrimarySelectedQuad();
    if (!quad) {
        setStatus("Pick Surface: select a quad to warp first", 5.0f);
        return;
    }
    const Corners corners = photo_.proposal->corners;

    GrayMapFn grayMap;
    OneFlashFn oneFlash;
    const PickMapping mapping = photoMappings(outputs, grayMap, oneFlash);
    if (mapping != PickMapping::CameraAtProjector && !sidecarReady()) {
        setStatus("Pick Surface: turn on AI Assist first (AI > Enable AI Assist)", 5.0f);
        return;
    }
    // Through startRequest even in photo space, so the lock and deleted-quad rules apply
    // in one place (applyOutcome).
    startRequest(mapping == PickMapping::CameraAtProjector ? "Pick Surface: applying..."
                                                          : "Pick Surface: mapping into projector space...",
                 PICK, quad->getId(), [corners, grayMap, oneFlash]() -> Outcome {
        return mapPhotoCorners("Pick Surface: ", corners, grayMap, oneFlash);
    });
}

void AiAssist::clearProposal() {
    photo_.proposal.reset();
    setStatus("Pick Surface: proposal cleared", 4.0f);
}

// ---- Scan Output (ADR-053 §3) ----------------------------------------------------------

bool AiAssist::canScan() const {
    if (!sidecarReady()) return false;
    const std::vector<std::string> caps = sidecar_->capabilities();
    return std::find(caps.begin(), caps.end(), "graycode") != caps.end();
}

bool AiAssist::grayMapCurrent(const GrayMap& map, const std::optional<ScanOutput>& now) {
    return !map.path.empty() && now && *now == map.output;
}

const AiAssist::GrayMap* AiAssist::currentGrayMap(const UUID& outputId,
                                                  const OutputManager* outputs) const {
    const auto it = grayMaps_.find(outputId);
    if (it == grayMaps_.end() || !outputs) return nullptr;
    const OutputWindow* output = outputs->getOutput(outputId);
    std::optional<ScanOutput> now;
    if (output) now = performedOutput(*output, outputs->getMonitors());
    return grayMapCurrent(it->second, now) ? &it->second : nullptr;
}

ScanOutput AiAssist::performedOutput(const OutputWindow& output, const std::vector<MonitorInfo>& monitors) {
    // In perform mode the output fills its display at the display's mode, whatever size
    // its window has when windowed.
    const int monitor = output.getMonitor();
    if (monitor >= 0 && monitor < static_cast<int>(monitors.size())) {
        return ScanOutput{monitors[monitor].resolution, monitor};
    }
    return ScanOutput{output.getResolution(), monitor};
}

void AiAssist::scanOutput(OutputManager* outputs) {
    if (refusedByPerformLock(editor_, "AI assist")) return;
    if (isBusy()) {
        setStatus("Scan: wait for the running request to finish", 5.0f);
        return;
    }
    if (!outputs) return;
    if (outputs->isBlackout()) {  // the camera would photograph black
        setStatus("Scan: Blackout is on - turn it off to scan", 6.0f);
        return;
    }
    // Calibration and slice warp editing own Esc and the canvas; a scan needs Esc.
    if (!editor_.calibratingOutputId.isNull()) {
        setStatus("Scan: leave calibration first", 5.0f);
        return;
    }
    if (!editor_.sliceWarpOutputId.isNull()) {
        setStatus("Scan: finish editing the slice warp first", 5.0f);
        return;
    }
    if (!sidecarReady()) {
        setStatus("Scan: turn on AI Assist first (AI > Enable AI Assist)", 5.0f);
        return;
    }
    if (!canScan()) {
        setStatus("Scan: the AI features are out of date: update or reinstall them", 6.0f);
        return;
    }
    // The selected output, else the first enabled one.
    OutputWindow* target = editor_.selectedOutputId.isNull()
                               ? nullptr : outputs->getOutput(editor_.selectedOutputId);
    if (!target) {
        for (const auto& output : outputs->getOutputs()) {
            if (output->isEnabled()) {
                target = output.get();
                break;
            }
        }
    }
    if (!target) {
        setStatus("Scan: add a projector output first (Output panel)", 5.0f);
        return;
    }
    if (!target->isEnabled()) {
        setStatus("Scan: enable output '" + target->getName() + "' first", 5.0f);
        return;
    }
    const IVec2 resolution = target->getResolution();
    if (resolution.x < 2 || resolution.y < 2) {
        setStatus("Scan: output '" + target->getName() + "' has no resolution", 5.0f);
        return;
    }

    // A fresh folder: the last scan's frames and captures go, and so does the map made
    // from them.
    const UUID outputId = target->getId();
    const std::filesystem::path folder = scanFolder(outputId);
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        setStatus("Scan: cannot create the scan folder " + utf8FromPath(folder), 6.0f);
        return;
    }
    grayMaps_.erase(outputId);

    // The camera opens now; the scan's first tick goes fullscreen and asks the sidecar
    // for the frames at the size the output shows there (ScanHost::requestPatterns).
    scanOutputId_ = outputId;
    scanHost_ = std::make_unique<ScanHostImpl>(*this, *outputs, folder);
    scan_ = std::make_unique<GrayCodeScan>(*scanHost_, openScanCamera(), outputId,
                                           performedOutput(*target, outputs->getMonitors()), folder);
    scanProgress_.clear();
    editor_.aiScan.running = true;
    setStatus("Scan: starting...", BUSY_STATUS_SECONDS);
}

void AiAssist::cancelScan() {
    if (!scan_) return;
    scan_->cancel();  // tick() posts the status and lets it go
}

void AiAssist::startScanDecode() {
    const UUID outputId = scan_->outputId();
    const ScanOutput output = scan_->output();
    const IVec2 size = scan_->patternSize();  // what the frames were made for
    const std::string folder = utf8FromPath(scan_->folder());
    scan_.reset();
    scanHost_.reset();
    scanProgress_.clear();
    std::shared_ptr<ai::SidecarClient> sidecar = sidecar_;
    if (!sidecar) return;
    startRequest("Scan: decoding the map...", SCAN, UUID::null(),
                 [sidecar, folder, outputId, output, size]() -> Outcome {
        return scanDecodeOutcome(sidecar->requestGrayCode(folder, size.x, size.y), outputId, output);
    });
}

void AiAssist::calibrationPixelAt(float x, float y, const OutputManager* outputs) {
    if (refusedByPerformLock(editor_, CALIBRATION)) return;
    const UUID outputId = editor_.calibratingOutputId;
    const OutputWindow* output = outputs && !outputId.isNull() ? outputs->getOutput(outputId) : nullptr;
    if (!output) {
        setStatus("Calibration: calibrate a projector first", 5.0f);
        return;
    }
    const int index = editor_.calibration.selected;
    const auto& points = output->getCalibrationPoints();
    if (index < 0 || index >= static_cast<int>(points.size())) {
        setStatus("Calibration: select a calibration point first", 5.0f);
        return;
    }
    const GrayMap* map = currentGrayMap(outputId, outputs);
    if (!map) {
        setStatus("Calibration: scan this projector first (AI > Scan Output)", 6.0f);
        return;
    }
    if (photo_.path.empty()) {
        setStatus("Calibration: take a photo first (AI Photo panel)", 5.0f);
        return;
    }
    if (!sidecarReady()) {
        setStatus("Calibration: turn on AI Assist first (AI > Enable AI Assist)", 5.0f);
        return;
    }
    if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return;  // not on the photo

    std::shared_ptr<ai::SidecarClient> sidecar = sidecar_;
    const CalibrationPixel target{outputId, index, points[index].world, {}};
    startRequest("Calibration: finding the point on the projector...", CALIBRATION, UUID::null(),
                 [sidecar, path = map->path, point = std::array<float, 2>{x, y}, target]() -> Outcome {
        const ai::GrayReply<ai::GrayMapResult> reply = sidecar->requestGrayMap(path, {point});
        if (reply.endpointMissing) {
            return Outcome{"Calibration: the AI features are out of date: update or reinstall them", 6.0f,
                           std::nullopt};
        }
        if (!reply.result) {
            return Outcome{"Calibration: the scan could not map the click" +
                               (reply.error.empty() ? std::string() : " (" + reply.error + ")"),
                           6.0f, std::nullopt};
        }
        if (!reply.result->valid[0]) {
            return Outcome{"Calibration: that spot is outside the scanned area - click inside it",
                           6.0f, std::nullopt};
        }
        Outcome outcome{formatStatus("Calibration: point %d placed from the photo - nudge with Tab + arrows",
                                     target.index + 1),
                        6.0f, std::nullopt};
        outcome.calibrationPixel = target;
        outcome.calibrationPixel->projector = reply.result->points[0];
        return outcome;
    });
}

bool AiAssist::applyCalibrationPixel(const CalibrationPixel& pixel, OutputManager* outputs) {
    OutputWindow* output = outputs ? outputs->getOutput(pixel.outputId) : nullptr;
    if (!output || editor_.calibratingOutputId != pixel.outputId) {
        setStatus("Calibration: result dropped - calibration of that projector ended", 6.0f);
        return false;
    }
    CalibrationSession& cal = editor_.calibration;
    if (cal.pick != CalibrationSession::Pick::Photo || cal.selected != pixel.index) {
        setStatus("Calibration: result dropped - the photo pick was cancelled or moved on", 6.0f);
        return false;
    }
    auto& points = output->getCalibrationPoints();
    if (pixel.index >= static_cast<int>(points.size()) || points[pixel.index].world != pixel.world) {
        setStatus("Calibration: result dropped - the point was changed meanwhile", 6.0f);
        return false;
    }
    // The canvas shows the output's frame edge to edge, so its normalized position is
    // the projector pixel's (canvasToPixel); an ordinary edit, one undo step, which
    // CalibrationPanel records with the live solve.
    points[pixel.index].pixel =
        canvasToPixel(Vec2(pixel.projector[0], pixel.projector[1]), output->getProjector().intrinsics);
    cal.selected = pixel.index;
    if (cal.pick == CalibrationSession::Pick::Photo) cal.pick = CalibrationSession::Pick::None;
    cal.noteEdit("Place calibration point " + std::to_string(pixel.index + 1) + " from the AI photo");
    cal.markChanged();
    return true;
}

bool AiAssist::renderPatternFrames(OutputManager& outputs, Renderer& renderer) {
    if (scanFrameShown_) {
        ensurePatternQuad();
        // Preparing (no frame yet): `only` matches no output, so every output is black.
        static const UUID NO_OUTPUT = UUID::generate();
        outputs.renderPatternFrames(*calibQuad_, scanTexture_.get(), renderer,
                                    scanTexture_ ? scanOutputId_ : NO_OUTPUT);
        return true;
    }
    if (!emitPattern_ || !calibQuad_) return false;
    outputs.renderPatternFrames(*calibQuad_, calibTexture_.get(), renderer);
    return true;
}

void AiAssist::stopSidecar() {
    // A scan gives the outputs back while they still exist.
    if (scan_) {
        scan_->cancel();
        scan_.reset();
        scanHost_.reset();
        editor_.aiScan.running = false;
    }
    if (!sidecar_) return;
    // Kill any launched sidecar now (no orphan), even while a running request still
    // holds a share of the client: its HTTP call then fails fast instead of waiting
    // out the request timeout.
    sidecar_->stop();
    sidecar_.reset();
}

} // namespace quadmapper
