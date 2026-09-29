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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace quadmapper {

namespace {

void showStatus(EditorState& es, const std::string& msg, float seconds) {
    snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
    es.statusTimer = seconds;
}

}  // namespace

AiAssist::AiAssist(EditorState& editor, std::unique_ptr<ai::SidecarClient> sidecar)
    : editor_(editor),
      sidecar_(std::move(sidecar)),
      lastSidecarState_(ai::SidecarState::Stopped),
      selfTestPending_(std::getenv("QM_AI_SELFTEST") != nullptr) {}

AiAssist::~AiAssist() = default;

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

    // Resolve the input image. QM_AI_TEST_IMAGE feeds a fixed photo (verification,
    // and the "detect from a phone photo on disk" path); otherwise capture a live
    // frame off the first camera (the QM-055 rail). NOTE: this blocks the UI for
    // ~1-3s (camera open + capture + inference) — acceptable pre-show; async is
    // tracked as TD-10.
    std::filesystem::path imgPath;
    if (const char* testImg = std::getenv("QM_AI_TEST_IMAGE")) {
        imgPath = testImg;
        if (!std::filesystem::exists(imgPath)) {
            setStatus("AI: QM_AI_TEST_IMAGE not found", 5.0f);
            return;
        }
    } else {
        imgPath = std::filesystem::temp_directory_path() / "quadmapper_ai_capture.png";
        std::string err;
        if (!captureCameraFrameToPng(utf8FromPath(imgPath), err)) {
            setStatus("AI: " + err, 5.0f);
            return;
        }
    }

    auto surface = sidecar_->requestSurface(utf8FromPath(imgPath));
    if (!surface) {
        setStatus("AI: detection failed (sidecar error)", 5.0f);
        return;
    }

    // Low confidence: do NOT auto-warp onto a possibly-wrong surface — ask the
    // user to disambiguate instead (the spike's load-bearing human-confirm step).
    constexpr float kConfidenceThreshold = 0.35f;
    if (surface->confidence < kConfidenceThreshold) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "AI: low confidence (%.2f) - tap the surface to retry",
                 surface->confidence);
        setStatus(msg, 5.0f);
        return;
    }

    // Apply the proposed corners (TL,TR,BR,BL, normalized [0,1] — same order and
    // space as QuadGeometry). The result is an ordinary warp: drawn on canvas,
    // F11-projectable, and refinable with Tab-cycle + arrow-nudge.
    std::array<Vec2, 4> corners{
        Vec2{surface->corners[0][0], surface->corners[0][1]},
        Vec2{surface->corners[1][0], surface->corners[1][1]},
        Vec2{surface->corners[2][0], surface->corners[2][1]},
        Vec2{surface->corners[3][0], surface->corners[3][1]},
    };
    quad->getGeometry().setCorners(corners);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "AI: surface applied (confidence %.2f) - nudge with Tab + arrows",
             surface->confidence);
    setStatus(msg, 5.0f);
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
    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "quadmapper_oneflash_capture.png";
    std::string err;
    if (!captureCameraFrameToPng(utf8FromPath(out), err)) {
        setStatus("One-Flash: " + err, 6.0f);
        return;
    }
    setStatus("One-Flash: captured " + utf8FromPath(out), 6.0f);
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
    // verification) else a live camera shot of the projected pattern.
    std::filesystem::path patternPath;
    if (const char* testImg = std::getenv("QM_AI_TEST_IMAGE")) {
        patternPath = testImg;
        if (!std::filesystem::exists(patternPath)) {
            setStatus("One-Flash: QM_AI_TEST_IMAGE not found", 6.0f);
            return;
        }
    } else {
        patternPath = std::filesystem::temp_directory_path() /
                      "quadmapper_oneflash_capture.png";
        std::string err;
        if (!captureCameraFrameToPng(utf8FromPath(patternPath), err)) {
            setStatus("One-Flash: " + err, 6.0f);
            return;
        }
    }

    auto result = sidecar_->requestOneFlash(utf8FromPath(patternPath), audienceTarget);
    if (!result || result->stub) {
        setStatus("One-Flash: pattern not detected (need the ChArUco visible)", 6.0f);
        return;
    }
    constexpr float kConfidenceThreshold = 0.35f;
    if (result->confidence < kConfidenceThreshold) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "One-Flash: low confidence (%.2f) - reframe the pattern",
                 result->confidence);
        setStatus(msg, 6.0f);
        return;
    }

    std::array<Vec2, 4> corners{
        Vec2{result->corners[0][0], result->corners[0][1]},
        Vec2{result->corners[1][0], result->corners[1][1]},
        Vec2{result->corners[2][0], result->corners[2][1]},
        Vec2{result->corners[3][0], result->corners[3][1]},
    };
    quad->getGeometry().setCorners(corners);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "One-Flash: %s (confidence %.2f) - send to projector (F11)",
             label, result->confidence);
    setStatus(msg, 6.0f);
}

bool AiAssist::renderPatternFrames(OutputManager& outputs, Renderer& renderer) {
    if (!emitPattern_ || !calibQuad_) return false;
    outputs.renderPatternFrames(*calibQuad_, calibTexture_.get(), renderer);
    return true;
}

void AiAssist::stopSidecar() {
    sidecar_.reset();  // stop()s / kills any launched sidecar (no orphan)
}

} // namespace quadmapper
