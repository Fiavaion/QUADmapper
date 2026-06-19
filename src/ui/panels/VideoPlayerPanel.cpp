#include "VideoPlayerPanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "app/MediaAssign.hpp"
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

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

namespace quadmapper {

namespace {

// Native "open video file" dialog (Win32 comdlg32 — no new dependency). Returns
// true and writes the chosen path into out on success; false if cancelled or
// unsupported. initial seeds the starting file/folder so it reopens where you were.
bool pickVideoFile(char* out, size_t outSize, const char* initial) {
#ifdef _WIN32
    char filename[MAX_PATH] = "";
    if (initial && initial[0] != '\0') {
        strncpy(filename, initial, sizeof(filename) - 1);
    }
    // Filter blocks are NUL-delimited and the whole thing is NUL-terminated.
    static const char filter[] =
        "Video Files\0*.mp4;*.mov;*.avi;*.mkv;*.webm;*.m4v;*.wmv;*.flv\0"
        "All Files\0*.*\0";

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = sizeof(filename);
    ofn.lpstrTitle = "Load Video";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (GetOpenFileNameA(&ofn)) {
        strncpy(out, filename, outSize - 1);
        out[outSize - 1] = '\0';
        return true;
    }
    return false;
#else
    (void)out; (void)outSize; (void)initial;
    return false;
#endif
}

}  // namespace

// ADR-040 P3: the Video Player drives one active VideoFileSource owned by the
// SourceManager. "Load" creates the source; controls drive it through the
// PlayableSource interface; "Assign" binds the source to a quad by mediaId — and
// because mediaId is just a reference, the SAME video can be assigned to many
// quads (each with its own crop) and decodes/uploads exactly once.
void VideoPlayerPanel::render(EditorContext& ctx) {
    EditorState& es = ctx.es;

    if (!es.showVideoPlayer) return;

    ImGui::Begin("Video Player");

    // Simple mode (ADR-042): Load Video... (browses + assigns to the selected quad),
    // then just Play/Pause, Loop and Speed — nothing else.
    if (es.uiMode == EditorState::UiMode::Simple) {
        Quad* selected = es.quadManager().getPrimarySelectedQuad();
        ImGui::TextDisabled(selected ? "Loads onto the selected quad"
                                     : "Select a quad first, then load");

        if (ImGui::Button("Load Video...")) {
            char picked[sizeof(es.videoPath)] = "";
            if (pickVideoFile(picked, sizeof(picked), es.videoPath) && ctx.sourceManager) {
                strncpy(es.videoPath, picked, sizeof(es.videoPath) - 1);
                es.videoPath[sizeof(es.videoPath) - 1] = '\0';
                UUID sid = ctx.sourceManager->getOrCreateVideoSource(es.videoPath);
                if (!sid.isNull()) {
                    es.activeVideoSourceId = sid;
                    if (selected) {
                        bindSourceToQuad(es, ctx.project, *selected, sid,
                                         es.videoPath, /*isVideo=*/true);
                    }
                    // Auto-play so the video is visible at once (a paused video uploads
                    // no frames, leaving the quad black).
                    if (auto* vs = ctx.sourceManager->getSourceAs<VideoFileSource>(sid)) {
                        vs->play();
                    }
                    snprintf(es.statusMessage, sizeof(es.statusMessage), "Loaded video");
                    es.statusTimer = 3.0f;
                }
            }
        }
        ImGui::SetItemTooltip("Browse for a video and apply it to the selected quad");

        VideoFileSource* v = ctx.sourceManager
            ? ctx.sourceManager->getSourceAs<VideoFileSource>(es.activeVideoSourceId)
            : nullptr;
        if (v) {
            ImGui::Separator();
            const bool playing = v->getPlaybackState() == PlaybackState::PLAYING;
            if (ImGui::Button(playing ? "Pause" : "Play")) {
                if (playing) v->pause(); else v->play();
            }
            bool loop = v->getLoopMode() == LoopMode::LOOP;
            if (ImGui::Checkbox("Loop", &loop)) {
                v->setLoopMode(loop ? LoopMode::LOOP : LoopMode::ONCE);
            }
            float speed = v->getSpeed();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat("Speed", &speed, 0.1f, 4.0f, "%.1fx")) {
                v->setSpeed(speed);
            }
            ImGui::SameLine();
            if (ImGui::Button("1x")) v->setSpeed(1.0f);
        } else {
            ImGui::TextDisabled("No video loaded");
        }

        ImGui::End();
        return;
    }

    // Load a video from es.videoPath through the SourceManager (shared by the
    // file-picker and the typed-path fallback).
    auto loadFromPath = [&]() {
        if (strlen(es.videoPath) > 0 && ctx.sourceManager) {
            UUID sid = ctx.sourceManager->createVideoSource(es.videoPath);
            if (!sid.isNull()) {
                es.activeVideoSourceId = sid;
                snprintf(es.statusMessage, sizeof(es.statusMessage),
                         "Video loaded: %s", es.videoPath);
            } else {
                snprintf(es.statusMessage, sizeof(es.statusMessage),
                         "Failed to load video: %s", es.videoPath);
            }
            es.statusTimer = 3.0f;
        }
    };

    // Primary path: "Load Video" opens a native file explorer; picking a file sets
    // the path field and loads it immediately. The text field still shows the path
    // (and a typed/pasted path can be loaded with the small "Use Path" fallback).
    ImGui::Text("Video File:");
    if (ImGui::Button("Load Video...")) {
        char picked[sizeof(es.videoPath)] = "";
        if (pickVideoFile(picked, sizeof(picked), es.videoPath)) {
            strncpy(es.videoPath, picked, sizeof(es.videoPath) - 1);
            es.videoPath[sizeof(es.videoPath) - 1] = '\0';
            loadFromPath();
        }
    }
    ImGui::SetItemTooltip("Browse the hard drive for a video file");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-70.0f);
    const bool pathEntered = ImGui::InputText("##videopath", es.videoPath, sizeof(es.videoPath),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Use Path") || pathEntered) {
        loadFromPath();
    }

    VideoFileSource* video = ctx.sourceManager
        ? ctx.sourceManager->getSourceAs<VideoFileSource>(es.activeVideoSourceId)
        : nullptr;

    if (video) {
        const VideoInfo& info = video->getVideoInfo();

        ImGui::Separator();
        ImGui::Text("Size: %dx%d | FPS: %.2f | Duration: %.1fs",
                    info.width, info.height, info.frameRate, info.duration);
        if (info.hasAudio) {
            ImGui::Text("Audio: %d ch @ %d Hz", info.audioChannels, info.audioSampleRate);
        }

        ImGui::Separator();

        // Playback controls
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

        float progress = video->getProgress();
        if (ImGui::SliderFloat("Progress", &progress, 0.0f, 1.0f, "%.1f%%")) {
            video->seekProgress(progress);
        }
        ImGui::Text("Time: %.1f / %.1f s", video->getCurrentTime(), video->getDuration());

        ImGui::Separator();

        // Speed
        float speed = video->getSpeed();
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 4.0f, "%.1fx")) {
            video->setSpeed(speed);
        }
        ImGui::SameLine();
        if (ImGui::Button("1x")) video->setSpeed(1.0f);

        // Loop mode
        const char* loopModes[] = { "Once", "Loop", "Ping-Pong" };
        int loopMode = static_cast<int>(video->getLoopMode());
        if (ImGui::Combo("Loop Mode", &loopMode, loopModes, 3)) {
            video->setLoopMode(static_cast<LoopMode>(loopMode));
        }

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
                                     video->getPath().string(), /*isVideo=*/true);
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
            ctx.sourceManager->removeSource(es.activeVideoSourceId);
            es.activeVideoSourceId = UUID();
        }
    } else {
        ImGui::TextDisabled("No video loaded");
        ImGui::TextDisabled("Click \"Load Video...\" to browse for a file");
    }

    ImGui::End();
}

}  // namespace quadmapper
