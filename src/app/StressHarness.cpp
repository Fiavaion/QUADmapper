// QM-081 perform-mode stress / leak harness (--stress), moved out of Application.cpp.

#include "StressHarness.hpp"
#include "Application.hpp"
#include "../editor/EditorState.hpp"
#include "../editor/TextureResolve.hpp"
#include "../editor/SourceAudio.hpp"
#include "../audio/AudioEngine.hpp"
#include "../audio/AudioMixer.hpp"
#include "../core/GlObjectStats.hpp"
#include "../core/Logger.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../renderer/DxInterop.hpp"
#include "../scene/Quad.hpp"
#include "../scene/QuadManager.hpp"
#include "../scene/Scene.hpp"
#include "../source/SourceManager.hpp"
#include "../source/VideoFileSource.hpp"
#include "../source/procedural/ProceduralSource.hpp"
#include "../video/VideoDecoder.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

// psapi is a Windows system library (no vcpkg dependency). NOMINMAX keeps
// windows.h from defining min/max macros that clash with std::max.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

namespace quadmapper {

bool parseStressArg(const char* arg, std::optional<StressConfig>& cfg) {
    // Intentionally minimal: no getopt dependency, no env-var fallback.
    auto config = [&cfg]() -> StressConfig& {
        if (!cfg) cfg = StressConfig{};
        return *cfg;
    };
    if (std::strcmp(arg, "--stress") == 0) {
        config();
    } else if (std::strncmp(arg, "--cycles=", 9) == 0) {
        config().cycles = std::atoi(arg + 9);
    } else if (std::strncmp(arg, "--outputs=", 10) == 0) {
        config().outputs = std::atoi(arg + 10);
    } else if (std::strncmp(arg, "--frames-per-cycle=", 19) == 0) {
        config().framesPerCycle = std::atoi(arg + 19);
    } else if (std::strcmp(arg, "--fullscreen") == 0) {
        config().fullscreen = true;
    } else if (std::strncmp(arg, "--source=", 9) == 0) {
        config().source = arg + 9;
    } else if (std::strcmp(arg, "--hidden") == 0) {
        config().hidden = true;
    } else if (std::strncmp(arg, "--ws-bound-mb=", 14) == 0) {
        config().workingSetBoundMB = std::atol(arg + 14);
    } else if (std::strncmp(arg, "--cycle-timeout-ms=", 19) == 0) {
        config().cycleTimeoutMs = std::atoi(arg + 19);
    } else if (std::strncmp(arg, "--pause-after=", 14) == 0) {
        config().pauseAfter = std::atoi(arg + 14);
    } else if (std::strncmp(arg, "--capture=", 10) == 0) {
        config().capture = arg + 10;
    } else if (std::strncmp(arg, "--media=", 8) == 0) {
        config().media = arg + 8;
    } else if (std::strcmp(arg, "--audio-churn") == 0) {
        config().audioChurn = true;
    } else {
        return false;
    }
    return true;
}

std::size_t processWorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return 0;
    }
    return pmc.WorkingSetSize;
}

// ---------------------------------------------------------------------------
// runStress — QM-081 H2/H3 headless perform-mode stress / leak harness.
//
// Control flow:
//   1. Build cfg.outputs output windows + one scene/quad + a procedural
//      TestPattern source (no file dependency).  Built once, before the loop.
//   2. Run ONE warm-up cycle (enter → N frames → exit) to let the output
//      frames (ADR-054) and textures settle.
//   3. Snapshot GlObjectStats::Snapshot::capture() + working set immediately
//      after warm-up exit — the consistent "post-exit" phase.
//   4. Loop cfg.cycles: enter → N frames (with wedge watchdog) → exit →
//      snapshot post-exit.  Compare every snapshot to baseline; track worst.
//   5. Print the diff table + working-set delta + STRESS PASS / STRESS FAIL.
//      Return 0 only when all gates pass.
//
// H3 — --hidden mode:
//   Exercises the same GL-object churn (output frame render targets resized
//   around enterPerformMode/exitPerformMode + renderOutputFrames) WITHOUT touching
//   borderless-fullscreen or bringing output windows on-screen.  Output
//   windows stay hidden via OutputWindow::hide(); the main window is also
//   hidden by glfwHideWindow at the start of the run.
//   What --hidden skips vs the real path:
//     - No glfwShowWindow / goBorderlessFullscreen / goWindowed calls.
//     - No monitor assignment / DWM composition swap.
//   What it still exercises (the leak-relevant part):
//     - Output frame lifecycle (each output's render target, drawn every frame
//       whether its window is shown or not) — the most common resource-leak site.
//     - All RAII wrapper create/destroy paths tracked by GlObjectStats.
//   presentOutputs skips hidden outputs, so --hidden never copies or swaps
//   them; run without --hidden to exercise (and time) that path.
// ---------------------------------------------------------------------------


int Application::runStress(const StressConfig& cfg) {
    Logger::info("[STRESS] QM-081 H4 — cycles={} outputs={} frames={} "
                 "source={} media={} fullscreen={} hidden={} ws-bound-mb={} "
                 "cycle-timeout-ms={} pause-after={}",
                 cfg.cycles, cfg.outputs, cfg.framesPerCycle,
                 cfg.source, cfg.media.empty() ? "(none)" : cfg.media,
                 cfg.fullscreen ? "true" : "false",
                 cfg.hidden     ? "true" : "false",
                 cfg.workingSetBoundMB, cfg.cycleTimeoutMs,
                 cfg.pauseAfter);

    // ---- 1. Build outputs ---------------------------------------------------
    // The default output was already created by initializeComponents().  Create
    // additional ones if cfg.outputs > 1.
    while (static_cast<int>(m_outputManager->getOutputCount()) < cfg.outputs) {
        int idx = static_cast<int>(m_outputManager->getOutputCount());
        OutputWindow* w = m_outputManager->createOutput("Stress-" + std::to_string(idx));
        if (!w) {
            Logger::error("[STRESS] Failed to create output window {}", idx);
            return 2;
        }
    }
    // Explicitly choose monitor 0 for every stress output, the default one too —
    // the dev box is safe, and an automatic choice would leave a one-monitor box
    // with nothing to show (QM-090).
    for (auto& ow : m_outputManager->getOutputs()) {
        m_outputManager->assignMonitor(*ow, 0);
    }

    // H3: In --hidden mode keep the main editor window off-screen too.
    if (cfg.hidden) {
        glfwHideWindow(m_window);
    }

    // ---- 2. Build one scene with one quad + source --------------------------
    EditorState& es = *m_editorState;

    // Guarantee at least one scene and one quad.
    Scene& scene = es.activeScene();
    QuadManager& qm = scene.getQuadManager();
    if (qm.getQuads().empty()) {
        qm.createQuad("Stress-Quad");
    }
    Quad* quad = qm.getQuads().front().get();

    // H4: route source creation by --source type.
    // For "video" and "hap", createVideoSource auto-routes to HapFileSource
    // when the container is a HAP .mov (via isHapContainer).
    // "procedural" (default) needs no file.
    UUID sourceId;
    PlayableSource* playable = nullptr;  // non-null when source supports play/pause

    const bool needsMedia = (cfg.source == "video" || cfg.source == "hap");
    if (needsMedia) {
        if (cfg.media.empty()) {
            Logger::error("[STRESS] --source={} requires --media=PATH; no path provided.",
                          cfg.source);
            return 2;
        }
        sourceId = m_sourceManager->createVideoSource(cfg.media);
        if (sourceId.isNull()) {
            Logger::error("[STRESS] Failed to create video/HAP source from '{}'", cfg.media);
            return 2;
        }
        playable = m_sourceManager->getSourceAs<PlayableSource>(sourceId);
        if (!playable) {
            Logger::error("[STRESS] Source created from '{}' is not a PlayableSource", cfg.media);
            return 2;
        }
        playable->play();
        Logger::info("[STRESS] Video/HAP source created from '{}'", cfg.media);
    } else {
        // Default: procedural TestPattern (no file, no network dependency).
        sourceId = m_sourceManager->createProceduralSource(ProceduralType::TEST_PATTERN,
                                                           320, 180);
        if (sourceId.isNull()) {
            Logger::error("[STRESS] Failed to create TestPattern source");
            return 2;
        }
    }
    quad->getTexture().mediaId = sourceId;

    // Show the active scene on output 0.
    const auto& outputs = m_outputManager->getOutputs();
    if (!outputs.empty()) {
        OutputManager::showScene(*outputs.front(), &scene, es.scenes);
    }

    // ---- Helper: one full cycle with wedge watchdog + pause/resume (H3/H4) --
    // Returns false if the cycle exceeded cfg.cycleTimeoutMs or the pause/resume
    // recovery check fails (LESSON-BUG-006 repro).
    bool pauseResumeWedge = false;
    bool audioLeak = false;  // --audio-churn: a replaced source kept its audio
    // QM-087 frame pacing: wall-clock time of the steady frame loop (enter/exit
    // transitions excluded), accumulated over the measured cycles only.
    bool measurePacing = false;
    double pacingSeconds = 0.0;
    long pacingFrames = 0;
    // A media source is up once it has shown a frame (so its textures, zero-copy
    // planes included, exist) and its audio is settled: no audio track, or an
    // attachment that has joined the mix. Its decoders start on their own threads, so
    // how many frames that takes depends on the machine's load; each cycle runs until
    // it is up, so every snapshot sees the source in the same state and every churned
    // source really builds its decoder thread and mixer input before it is freed.
    auto sourceUp = [&]() -> bool {
        const TextureSource* source = m_sourceManager->getSource(sourceId);
        if (!source || !source->getCurrentFrame()) return false;
        if (!m_audioEngine) return true;
        const SourceAudioStatus audio = m_audioEngine->status(sourceId);
        return audio == SourceAudioStatus::NoAudio ||
               (audio == SourceAudioStatus::Ready && m_audioEngine->mixer().getSourceCount() > 0);
    };
    auto runCycle = [&]() -> bool {
        const double budgetSec = cfg.cycleTimeoutMs / 1000.0;

        // Audio churn: a fresh video source (and so a fresh audio attachment) per cycle.
        // The first sync after the removal frees the old attachment.
        if (cfg.audioChurn && needsMedia) {
            m_sourceManager->removeSource(sourceId);
            sourceId = m_sourceManager->createVideoSource(cfg.media);
            playable = m_sourceManager->getSourceAs<PlayableSource>(sourceId);
            if (!playable) {
                Logger::error("[STRESS] Audio churn: could not re-create '{}'", cfg.media);
                return false;
            }
            playable->play();
            quad->getTexture().mediaId = sourceId;
        }

        // The wedge watchdog times the cycle from here. Opening a file and its hardware
        // decoder, above, took over 3 s while other GPU tests started (ctest -j 4): slow
        // under contention, not wedged. A call that never returns was never caught here
        // (the check runs between frames); the ctest TIMEOUT catches it.
        const double cycleStart = glfwGetTime();
        if (cfg.hidden) {
            // --hidden: call the same enter/exit path, but keep windows hidden.
            // enterPerformMode normally shows + fullscreens each window; hiding
            // them again right after skips that, while renderOutputFrames still
            // draws every output's frame regardless of visibility.
            m_outputManager->enterPerformMode(/*borderless=*/false);
            // Keep windows hidden after enterPerformMode showed them.
            for (auto& ow : m_outputManager->getOutputs()) {
                if (ow->isCreated()) {
                    ow->hide();
                }
            }
        } else {
            m_outputManager->enterPerformMode(cfg.fullscreen);
        }

        // H4 pause/resume state for this cycle.
        // pausedAt: frame index when we called pause(); -1 = not yet paused.
        // resumedAt: frame index when we called play(); -1 = not yet resumed.
        int pausedAt   = -1;
        int resumedAt  = -1;
        // Time just before the pause (for reporting if recovery check runs).
        double timeAtPause = 0.0;

        double t = glfwGetTime();
        const double loopStart = t;
        int frames = 0;
        for (int f = 0; f < cfg.framesPerCycle || (needsMedia && !sourceUp()); ++f) {
            // Wedge check: abort if we've been in this cycle too long, which includes a
            // media source that never comes up.
            if ((glfwGetTime() - cycleStart) > budgetSec) {
                if (needsMedia && !sourceUp()) {
                    Logger::error("[STRESS] The media source was not up (a frame shown, its audio in "
                                  "the mix) {} ms into the cycle.", cfg.cycleTimeoutMs);
                }
                m_outputManager->exitPerformMode();
                return false;
            }
            ++frames;

            glfwPollEvents();

            // H4: pause/resume logic — triggered only when pauseAfter > 0 and
            // the source is playable.
            if (playable && cfg.pauseAfter > 0) {
                if (pausedAt < 0 && f == cfg.pauseAfter) {
                    // Pause: the decode thread is now idle with frames potentially held.
                    timeAtPause = playable->getCurrentTime();
                    playable->pause();
                    pausedAt = f;
                    Logger::info("[STRESS] pause() at frame {} (t={}s)", f, timeAtPause);
                } else if (pausedAt >= 0 && resumedAt < 0
                           && (f - pausedAt) >= 5) {
                    // Hold paused for 5 frames, then resume.
                    // 5 frames is enough for the decode thread to drain its prefetch
                    // and sit idle, exercising the pool-hold condition.
                    playable->play();
                    resumedAt = f;
                    Logger::info("[STRESS] play() (resume) at frame {}", f);
                } else if (resumedAt >= 0 && (f - resumedAt) == 8) {
                    // Recovery check: 8 frames after resume the source must have
                    // advanced beyond the time it was at when we paused.
                    // (8 frames at any realistic FPS is well over 80ms — enough
                    // for the decode thread to wake and produce a new frame even
                    // under load.  Using == not >= so the check fires exactly once.
                    // With pauseAfter=15 and framesPerCycle=30: resume@20, check@28.
                    // If it hasn't advanced, the thread wedged.)
                    double timeAfterResume = playable->getCurrentTime();
                    PlaybackState state    = playable->getPlaybackState();
                    bool recovered = (state == PlaybackState::PLAYING)
                                  && (timeAfterResume > timeAtPause);
                    if (!recovered) {
                        Logger::error("[STRESS] STRESS FAIL (pause-resume wedge) — "
                                      "source did not recover after resume: "
                                      "state={} t_before={}s t_after={}s",
                                      static_cast<int>(state),
                                      timeAtPause, timeAfterResume);
                        pauseResumeWedge = true;
                        m_outputManager->exitPerformMode();
                        return false;
                    }
                    Logger::info("[STRESS] pause/resume recovered OK "
                                 "(t_before={}s t_after={}s)",
                                 timeAtPause, timeAfterResume);
                }
            }

            double now = glfwGetTime();
            double dt  = now - t;
            t = now;
            m_sourceManager->updateAll(dt);
            syncAudio();

            // Mirror run(): present the (vsync'd) editor window, then the outputs,
            // so the measured frame time includes the real swap chain.
            if (!cfg.hidden) {
                glfwSwapBuffers(m_window);
            }
            auto textures = buildQuadTextureMap(es, m_sourceManager.get(), SurfaceView::Output);
            m_outputManager->renderOutputFrames(es.scenes, textures, *m_renderer);
            m_outputManager->presentOutputs();
        }
        if (measurePacing) {
            pacingSeconds += glfwGetTime() - loopStart;
            pacingFrames += frames;
        }

        m_outputManager->exitPerformMode();
        if (cfg.audioChurn && m_audioEngine) {
            // Only this cycle's source may have audio: the replaced ones were freed.
            const size_t attachments = m_audioEngine->attachmentCount();
            const size_t inputs = m_audioEngine->mixer().getSourceCount();
            Logger::info("[STRESS] Audio: {} attachment(s), {} mixer input(s)", attachments, inputs);
            if (attachments > 1 || inputs > 1) {
                Logger::error("[STRESS] STRESS FAIL (audio) — replaced sources kept their audio");
                audioLeak = true;
                return false;
            }
        }
        return true;
    };

    // ---- 3. Warm-up cycle ---------------------------------------------------
    Logger::info("[STRESS] Warm-up cycle...");
    if (!runCycle()) {
        if (pauseResumeWedge) {
            Logger::error("[STRESS] STRESS FAIL (pause-resume wedge) — warm-up cycle failed.");
        } else if (!audioLeak) {
            Logger::error("[STRESS] STRESS FAIL (wedge) — warm-up cycle exceeded {}ms budget.",
                          cfg.cycleTimeoutMs);
        }
        return 1;
    }

    // Baseline captured immediately after warm-up exit (consistent post-exit phase).
    auto baseline   = GlObjectStats::Snapshot::capture();
    std::size_t wsBaseline = processWorkingSetBytes();

    Logger::info("[STRESS] Baseline captured after warm-up.  Working set: {} MB",
                 wsBaseline / (1024 * 1024));
    measurePacing = true;

    // ---- 4. Stress loop (with wedge watchdog + working-set tracking) --------
    GlObjectStats::Snapshot worst = baseline;
    bool anyLeak   = false;
    bool anyWedge  = false;
    bool wsExceeded = false;
    std::size_t wsMax   = wsBaseline;

    for (int c = 0; c < cfg.cycles; ++c) {
        if (!runCycle()) {
            // runCycle returns false for two reasons:
            // (a) wall-clock budget exceeded (anyWedge), OR
            // (b) pause-resume recovery check failed (pauseResumeWedge, already logged).
            if (!pauseResumeWedge && !audioLeak) {
                Logger::error("[STRESS] STRESS FAIL (wedge) — cycle {} exceeded {}ms budget.",
                              c + 1, cfg.cycleTimeoutMs);
                anyWedge = true;
            }
            break;
        }

        auto snap = GlObjectStats::Snapshot::capture();
        if (!snap.isFlat(baseline)) {
            anyLeak = true;
            // Track worst (highest counts in each category independently).
            worst.textures      = std::max(worst.textures,      snap.textures);
            worst.framebuffers  = std::max(worst.framebuffers,  snap.framebuffers);
            worst.vertexArrays  = std::max(worst.vertexArrays,  snap.vertexArrays);
            worst.vertexBuffers = std::max(worst.vertexBuffers, snap.vertexBuffers);
            worst.shaderPrograms= std::max(worst.shaderPrograms,snap.shaderPrograms);
        }

        // H3: working-set sample (spec §2).
        std::size_t wsCurrent = processWorkingSetBytes();
        if (wsCurrent > wsMax) {
            wsMax = wsCurrent;
        }
        long growthMB = static_cast<long>((wsCurrent - wsBaseline) / (1024 * 1024));
        if (growthMB > cfg.workingSetBoundMB) {
            wsExceeded = true;
            Logger::error("[STRESS] STRESS FAIL (working-set) — cycle {} grew {} MB "
                          "(bound {} MB).", c + 1, growthMB, cfg.workingSetBoundMB);
        }

        Logger::info("[STRESS] Cycle {}/{} complete — {} | WS={} MB (delta {} MB)",
                     c + 1, cfg.cycles,
                     snap.isFlat(baseline) ? "flat" : "DELTA DETECTED",
                     wsCurrent / (1024 * 1024),
                     growthMB);
    }

    // ---- 5. Report ----------------------------------------------------------
    Logger::info("[STRESS] Worst-case GL diff vs baseline:");
    Logger::info("{}", worst.diff(baseline));

    long finalGrowthMB = static_cast<long>((wsMax - wsBaseline) / (1024 * 1024));
    Logger::info("[STRESS] Working-set: baseline={} MB  max={} MB  peak-delta={} MB  "
                 "bound={} MB",
                 wsBaseline / (1024 * 1024),
                 wsMax      / (1024 * 1024),
                 finalGrowthMB,
                 cfg.workingSetBoundMB);
    if (pacingFrames > 0) {
        const double frameMs = pacingSeconds * 1000.0 / pacingFrames;
        Logger::info("[STRESS] Frame pacing: {} outputs, {} frames, avg {} ms/frame "
                     "({} fps){}",
                     cfg.outputs, pacingFrames,
                     static_cast<long>(frameMs * 100.0 + 0.5) / 100.0,
                     static_cast<long>(10000.0 / frameMs + 0.5) / 10.0,
                     cfg.hidden ? " [hidden: outputs not presented]" : "");
    }

    // H3: optional GPU memory report via GL_NVX_gpu_memory_info.
    // The extension is trivially detected at runtime; silently skip on AMD /
    // Intel / non-NVIDIA drivers.  Do NOT load GLAD GL_NVX_ — the extension is
    // query-only (no new entry points), so a plain glGetIntegerv is sufficient
    // once we confirm the extension string is present.
    //
    // GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX = 0x9049 (Khronos registry).
    if (glfwExtensionSupported("GL_NVX_gpu_memory_info")) {
        GLint availKB = 0;
        glGetIntegerv(0x9049 /*GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX*/, &availKB);
        Logger::info("[STRESS] GPU VRAM available (NVX): {} MB", availKB / 1024);
    }

    if (anyLeak) {
        Logger::error("[STRESS] STRESS FAIL (leak) — GL object count grew across cycles.");
    }
    if (wsExceeded) {
        Logger::error("[STRESS] STRESS FAIL (working-set) — peak growth exceeded {} MB bound.",
                      cfg.workingSetBoundMB);
    }
    if (pauseResumeWedge) {
        Logger::error("[STRESS] STRESS FAIL (pause-resume wedge) — source did not recover "
                      "after pause/resume (LESSON-BUG-006 repro).");
    }

    // ADR-056: once the video source is gone, nothing D3D11 is held for it: no interop
    // target (shared planes, opened frames, views, registrations), no decoded frame left
    // on the GPU, no readback staging texture or copy event query. Not counted: the
    // decoders' shared D3D11 device (ADR-055) and its surface pools, which FFmpeg frees
    // with each decoder, and the renderer's interop device, one per SourceManager, alive
    // until the app closes (at most one may remain).
    bool d3dLeak = false;
    if (needsMedia) {
        const auto* video = m_sourceManager->getSourceAs<VideoFileSource>(sourceId);
        const bool zeroCopy = video && video->isZeroCopy();
        m_sourceManager->removeSource(sourceId);
        quad->getTexture().mediaId = UUID();
        const long interopObjects = DxInterop::liveObjects();
        const long interopDevices = DxInterop::liveDevices();
        const long gpuFrames = VideoDecoder::liveGpuFrames();
        const long decoderObjects = VideoDecoder::liveD3dObjects();
        d3dLeak = interopObjects != 0 || gpuFrames != 0 || decoderObjects != 0 || interopDevices > 1;
        if (d3dLeak) {
            Logger::error("[STRESS] STRESS FAIL (D3D11 leak) — with the video source removed, {} interop "
                          "objects, {} interop devices, {} GPU frames and {} staging textures or queries "
                          "are still alive.", interopObjects, interopDevices, gpuFrames, decoderObjects);
        } else {
            Logger::info("[STRESS] D3D11 ({}): no interop objects, GPU frames, staging textures or queries "
                         "left after the source was removed; {} interop device kept.",
                         zeroCopy ? "zero-copy" : "not zero-copy", interopDevices);
        }
    }

    if (anyLeak || anyWedge || wsExceeded || pauseResumeWedge || audioLeak || d3dLeak) {
        return 1;
    }

    Logger::info("[STRESS] STRESS PASS — {} cycles, GL flat, WS delta {} MB (bound {} MB){}.",
                 cfg.cycles, finalGrowthMB, cfg.workingSetBoundMB,
                 (cfg.pauseAfter > 0 && playable) ? ", pause/resume OK" : "");
    return 0;
}

}  // namespace quadmapper
