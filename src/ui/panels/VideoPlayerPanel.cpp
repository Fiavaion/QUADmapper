#include "VideoPlayerPanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
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

namespace quadmapper {

// ADR-040 P3: the Video Player drives one active VideoFileSource owned by the
// SourceManager. "Load" creates the source; controls drive it through the
// PlayableSource interface; "Assign" binds the source to a quad by mediaId — and
// because mediaId is just a reference, the SAME video can be assigned to many
// quads (each with its own crop) and decodes/uploads exactly once.
void VideoPlayerPanel::render(EditorContext& ctx) {
    EditorState& es = ctx.es;

    if (!es.showVideoPlayer) return;

    ImGui::Begin("Video Player");

    ImGui::Text("Video File:");
    ImGui::InputText("##videopath", es.videoPath, sizeof(es.videoPath));
    ImGui::SameLine();
    if (ImGui::Button("Load Video")) {
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
                selected->getTexture().mediaId = isAssigned ? UUID() : es.activeVideoSourceId;
                ctx.project->setModified(true);
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
        ImGui::TextDisabled("Enter a video file path and click Load");
    }

    ImGui::End();
}

}  // namespace quadmapper
