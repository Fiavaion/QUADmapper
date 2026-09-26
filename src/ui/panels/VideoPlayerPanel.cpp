#include "VideoPlayerPanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "app/MediaAssign.hpp"
#include "app/SourceAudio.hpp"
#include "audio/AudioEngine.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "source/SourceManager.hpp"
#include "source/VideoFileSource.hpp"
#include "core/UUID.hpp"
#include "core/Types.hpp"
#include "project/Project.hpp"

#include <imgui.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace quadmapper {

namespace {

// Native "open video file" request; initial seeds the dialog so it reopens where you were.
AsyncFileDialog::Request videoFileRequest(const char* initial) {
    AsyncFileDialog::Request request;
    request.kind = AsyncFileDialog::Kind::OpenFile;
    request.title = "Load Video";
    // Filter blocks are NUL-delimited and the whole thing is NUL-terminated.
    static const char filter[] =
        "Video Files\0*.mp4;*.mov;*.avi;*.mkv;*.webm;*.m4v;*.wmv;*.flv\0"
        "All Files\0*.*\0";
    request.filter.assign(filter, sizeof(filter));
    request.initialPath = initial ? initial : "";
    return request;
}

// Decode failures surfaced by the source (QM-088), shown under the controls.
void showPlaybackError(const PlayableSource& video) {
    const std::string error = video.getErrorMessage();
    if (!error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", error.c_str());
    }
}

// Once / Loop / Ping-Pong (ADR-050), and why a requested Ping-Pong plays as Loop.
void loopModeSelector(PlayableSource& video, Project* project, float width) {
    static const char* const LOOP_MODES[] = {"Once", "Loop", "Ping-Pong"};  // LoopMode order
    int mode = static_cast<int>(video.getLoopMode());
    ImGui::SetNextItemWidth(width);
    if (ImGui::Combo("Loop Mode", &mode, LOOP_MODES, IM_ARRAYSIZE(LOOP_MODES))) {
        video.setLoopMode(static_cast<LoopMode>(mode));
        if (project) project->setModified(true);
    }
    ImGui::SetItemTooltip("Once stops on the last frame; Loop starts over; Ping-Pong plays "
                          "back and forth");
    const std::string notice = video.getLoopModeNotice();
    if (!notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
        ImGui::TextWrapped("Ping-Pong is playing as Loop: %s", notice.c_str());
        ImGui::PopStyleColor();
    }
}

// The source's sound (ADR-050 §2): mute and volume, and why it is silent when it is.
void audioControls(PlayableSource& video, const UUID& id, EditorContext& ctx, float width) {
    AudioEngine* audio = ctx.audio;
    Project* project = ctx.project;
    const SourceAudioStatus status = audio ? audio->status(id) : SourceAudioStatus::None;
    if (status == SourceAudioStatus::NoAudio) {
        ImGui::TextDisabled("No audio stream");
        return;
    }
    if (status != SourceAudioStatus::Ready) {
        ImGui::TextDisabled("Audio: opening...");
        return;
    }
    bool muted = video.isAudioMuted();
    if (ImGui::Checkbox("Mute##source audio", &muted)) {
        video.setAudioMuted(muted);
        if (project) project->setModified(true);
    }
    ImGui::SetItemTooltip("Mute this video's sound");
    ImGui::SameLine();
    float percent = video.getAudioVolume() * 100.0f;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("Volume##source audio", &percent, 0.0f, 100.0f, "%.0f%%")) {
        video.setAudioVolume(percent / 100.0f);
        if (project) project->setModified(true);
    }
    ImGui::SetItemTooltip("This video's volume (the menu bar's master volume applies on top)");
    if (const char* reason = audioSilenceReason(video)) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", reason);
        ImGui::SetItemTooltip("Sound plays only at 1x forward; it resumes in step at 1x");
    } else if (ctx.outputManager &&
               !audibleSourceIds(ctx.es.scenes, ctx.es.activeSceneId,
                                 enabledOutputIds(ctx.outputManager->getOutputs()))
                    .count(id)) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Silent: not in the active or a live scene");
        ImGui::SetItemTooltip("Heard only while a scene using it is the active scene or is assigned "
                              "to an enabled output; it stays in step meanwhile");
    }
}

// Timeline lock (ADR-050 §3): a locked source follows the master timeline in the menu
// bar, so its own transport is shown disabled.
void showLockedNote(const PlayableSource& video) {
    if (video.isTimelineLocked()) {
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f),
                           "Locked to the master timeline (menu bar: Timeline Play/Pause/Restart)");
    }
}

}  // namespace

void VideoPlayerPanel::loadVideo(EditorContext& ctx, bool simpleMode) {
    EditorState& es = ctx.es;
    if (es.videoPath[0] == '\0' || !ctx.sourceManager) return;

    // Deduped: loading a file that is already a source reuses it (one decode).
    UUID sid = ctx.sourceManager->getOrCreateVideoSource(es.videoPath);
    if (sid.isNull()) {
        snprintf(es.statusMessage, sizeof(es.statusMessage),
                 "Failed to load video: %s", es.videoPath);
        es.statusTimer = 3.0f;
        return;
    }
    es.activeVideoSourceId = sid;

    if (simpleMode) {
        if (Quad* selected = es.quadManager().getPrimarySelectedQuad()) {
            bindSourceToQuad(es, ctx.project, *selected, sid, es.videoPath, /*isVideo=*/true);
        }
        // Auto-play so the video is visible at once.
        if (auto* video = ctx.sourceManager->getSourceAs<PlayableSource>(sid)) {
            video->play();
        }
    }
    snprintf(es.statusMessage, sizeof(es.statusMessage), "Video loaded: %s", es.videoPath);
    es.statusTimer = 3.0f;
}

// ADR-040 P3: the Video Player drives one active playable source (FFmpeg-decoded or
// HAP) owned by the SourceManager. "Load" creates the source; controls drive it through
// the PlayableSource interface; "Assign" binds the source to a quad by mediaId — and
// because mediaId is just a reference, the SAME video can be assigned to many
// quads (each with its own crop) and decodes/uploads exactly once.
void VideoPlayerPanel::render(EditorContext& ctx) {
    EditorState& es = ctx.es;

    // Collect a finished "Load Video..." pick even while the window is hidden.
    std::vector<std::string> picked;
    if (loadDialog_.takeResult(picked) && !picked.empty()) {
        snprintf(es.videoPath, sizeof(es.videoPath), "%s", picked.front().c_str());
        loadVideo(ctx, loadDialogSimpleMode_);
    }

    if (!es.showVideoPlayer) return;

    ImGui::Begin("Video Player");

    const bool simpleMode = es.uiMode == EditorState::UiMode::Simple;
    auto loadButton = [&](const char* tooltip) {
        ImGui::BeginDisabled(loadDialog_.isBusy());
        if (ImGui::Button("Load Video...") && loadDialog_.open(videoFileRequest(es.videoPath))) {
            loadDialogSimpleMode_ = simpleMode;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", tooltip);
    };

    PlayableSource* video = ctx.sourceManager
        ? ctx.sourceManager->getSourceAs<PlayableSource>(es.activeVideoSourceId)
        : nullptr;

    // Simple mode (ADR-042): Load Video... (browses + assigns to the selected quad),
    // then just Play/Pause, Loop and Speed — nothing else.
    if (simpleMode) {
        Quad* selected = es.quadManager().getPrimarySelectedQuad();
        ImGui::TextDisabled(selected ? "Loads onto the selected quad"
                                     : "Select a quad first, then load");

        loadButton("Browse for a video and apply it to the selected quad");

        if (video) {
            ImGui::Separator();
            showLockedNote(*video);
            ImGui::BeginDisabled(video->isTimelineLocked());
            const bool playing = video->getPlaybackState() == PlaybackState::PLAYING;
            if (ImGui::Button(playing ? "Pause" : "Play")) {
                if (playing) video->pause(); else video->play();
            }
            ImGui::EndDisabled();
            loopModeSelector(*video, ctx.project, 180.0f);
            ImGui::BeginDisabled(video->isTimelineLocked());
            float speed = video->getSpeed();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat("Speed", &speed, 0.1f, 4.0f, "%.1fx")) {
                video->setSpeed(speed);
            }
            ImGui::SameLine();
            if (ImGui::Button("1x")) video->setSpeed(1.0f);
            ImGui::EndDisabled();
            audioControls(*video, es.activeVideoSourceId, ctx, 120.0f);
            showPlaybackError(*video);
        } else {
            ImGui::TextDisabled("No video loaded");
        }

        ImGui::End();
        return;
    }

    // Primary path: "Load Video" opens a native file explorer; picking a file sets
    // the path field and loads it. The text field still shows the path (and a
    // typed/pasted path can be loaded with the small "Use Path" fallback).
    ImGui::Text("Video File:");
    loadButton("Browse the hard drive for a video file");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-70.0f);
    const bool pathEntered = ImGui::InputText("##videopath", es.videoPath, sizeof(es.videoPath),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Use Path") || pathEntered) {
        loadVideo(ctx, /*simpleMode=*/false);
        video = ctx.sourceManager
            ? ctx.sourceManager->getSourceAs<PlayableSource>(es.activeVideoSourceId)
            : nullptr;
    }

    if (video) {
        ImGui::Separator();
        ImGui::Text("%s | Size: %dx%d | Duration: %.1fs", video->getTypeName().c_str(),
                    video->getWidth(), video->getHeight(), video->getDuration());
        // Decoder details only the FFmpeg-decoded source reports.
        if (const auto* decoded = dynamic_cast<const VideoFileSource*>(video)) {
            const VideoInfo& info = decoded->getVideoInfo();
            ImGui::Text("FPS: %.2f", info.frameRate);
            if (info.hasAudio) {
                ImGui::Text("Audio: %d ch @ %d Hz", info.audioChannels, info.audioSampleRate);
            }
        }

        ImGui::Separator();

        // Timeline lock: the source follows the master timeline instead of its own clock.
        bool locked = video->isTimelineLocked();
        if (ImGui::Checkbox("Lock to master timeline", &locked)) {
            ctx.sourceManager->setTimelineLocked(es.activeVideoSourceId, locked);
            if (ctx.project) ctx.project->setModified(true);
        }
        ImGui::SetItemTooltip("Follow the master timeline in the menu bar; its time is looped "
                              "over this clip, so locked videos stay in step");
        showLockedNote(*video);

        // Playback controls (the master timeline's, while locked)
        ImGui::BeginDisabled(locked);
        bool playing = video->getPlaybackState() == PlaybackState::PLAYING;
        if (playing) {
            if (ImGui::Button("Pause")) video->pause();
        } else {
            if (ImGui::Button("Play")) video->play();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) video->stop();
        ImGui::SameLine();
        if (ImGui::Button("<< 5s")) video->seek(video->getCurrentTime() - 5.0);
        ImGui::SameLine();
        if (ImGui::Button(">> 5s")) video->seek(video->getCurrentTime() + 5.0);

        // Shown as a percentage; the source works in a 0-1 fraction.
        float percent = video->getProgress() * 100.0f;
        if (ImGui::SliderFloat("Progress", &percent, 0.0f, 100.0f, "%.1f%%")) {
            video->seekProgress(percent / 100.0f);
        }
        ImGui::EndDisabled();
        const bool reversing = video->getClock().direction(video->getClockNow()) < 0 &&
                               video->getPlaybackState() == PlaybackState::PLAYING;
        ImGui::Text("Time: %.1f / %.1f s%s", video->getCurrentTime(), video->getDuration(),
                    reversing ? "  (reverse)" : "");
        showPlaybackError(*video);

        ImGui::Separator();

        // Speed
        ImGui::BeginDisabled(locked);
        float speed = video->getSpeed();
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 4.0f, "%.1fx")) {
            video->setSpeed(speed);
        }
        ImGui::SameLine();
        if (ImGui::Button("1x")) video->setSpeed(1.0f);
        ImGui::EndDisabled();

        loopModeSelector(*video, ctx.project, ImGui::CalcItemWidth());

        ImGui::Separator();
        ImGui::Text("Audio:");
        audioControls(*video, es.activeVideoSourceId, ctx, ImGui::CalcItemWidth() - 70.0f);

        ImGui::Separator();

        // Assign to one OR MANY quads — each binds this source by mediaId, so the
        // same decode feeds every assigned face. Crop each (Inspector) to wrap a box.
        ImGui::Text("Assign Video to Quad:");
        Quad* selected = es.quadManager().getPrimarySelectedQuad();
        if (selected) {
            bool isAssigned = (selected->getTexture().mediaId == es.activeVideoSourceId);
            if (ImGui::Button(isAssigned ? "Remove from Quad" : "Assign to Selected Quad")) {
                if (isAssigned) {
                    selected->getTexture().mediaId = UUID();
                    es.quadTexturePaths.erase(selected->getId());
                    ctx.project->setModified(true);
                } else {
                    // Centralized bind (ADR-042 P2): records the path so the assignment
                    // persists in the .qmap; load re-creates the source and re-binds.
                    bindSourceToQuad(es, ctx.project, *selected, es.activeVideoSourceId,
                                     ctx.sourceManager->getSourcePath(es.activeVideoSourceId).string(),
                                     /*isVideo=*/true);
                }
            }
            if (isAssigned) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                                   "Video on: %s", selected->getName().c_str());
            }
            ImGui::TextDisabled("Assign to several quads + crop each = one video wrapped");
        } else {
            ImGui::TextDisabled("Select a quad to assign video");
        }

        ImGui::Separator();
        if (ImGui::Button("Unload Video")) {
            // Take it off every quad too, so no binding is left pointing at nothing.
            const UUID unloaded = es.activeVideoSourceId;
            for (const auto& scene : es.scenes) {
                if (!scene) continue;
                for (const auto& quad : scene->getQuadManager().getQuads()) {
                    if (quad->getTexture().mediaId == unloaded) {
                        quad->getTexture().mediaId = UUID();
                        es.quadTexturePaths.erase(quad->getId());
                        ctx.project->setModified(true);
                    }
                }
            }
            ctx.sourceManager->removeSource(unloaded);
            es.activeVideoSourceId = UUID();
        }
    } else {
        ImGui::TextDisabled("No video loaded");
        ImGui::TextDisabled("Click \"Load Video...\" to browse for a file");
    }

    ImGui::End();
}

}  // namespace quadmapper
