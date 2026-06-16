#include "VideoPlayerPanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "video/VideoPlayer.hpp"
#include "core/UUID.hpp"
#include "project/Project.hpp"

#include <glad/glad.h>
#include <imgui.h>
#include <cstring>
#include <cstdio>

namespace quadmapper {

void VideoPlayerPanel::render(EditorContext& ctx) {
    EditorState& es = ctx.es;

    if (!es.showVideoPlayer) return;

    ImGui::Begin("Video Player");

    ImGui::Text("Video File:");
    ImGui::InputText("##videopath", es.videoPath, sizeof(es.videoPath));
    ImGui::SameLine();
    if (ImGui::Button("Load Video")) {
        if (strlen(es.videoPath) > 0) {
            if (es.videoPlayer.load(es.videoPath)) {
                snprintf(es.statusMessage, sizeof(es.statusMessage), "Video loaded: %s", es.videoPath);
            } else {
                snprintf(es.statusMessage, sizeof(es.statusMessage), "Failed to load video: %s", es.videoPath);
            }
            es.statusTimer = 3.0f;
        }
    }

    if (es.videoPlayer.isLoaded()) {
        const VideoInfo& info = es.videoPlayer.getVideoInfo();
        const VideoPlaybackState& state = es.videoPlayer.getState();

        ImGui::Separator();

        // Video info
        ImGui::Text("Size: %dx%d | FPS: %.2f | Duration: %.1fs",
                   info.width, info.height, info.frameRate, info.duration);
        if (info.hasAudio) {
            ImGui::Text("Audio: %d ch @ %d Hz", info.audioChannels, info.audioSampleRate);
        }

        ImGui::Separator();

        // Playback controls
        if (state.isPlaying()) {
            if (ImGui::Button("Pause")) {
                es.videoPlayer.pause();
            }
        } else {
            if (ImGui::Button("Play")) {
                es.videoPlayer.play();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            es.videoPlayer.stop();
        }
        ImGui::SameLine();
        if (ImGui::Button("<< 5s")) {
            es.videoPlayer.skip(-5.0);
        }
        ImGui::SameLine();
        if (ImGui::Button(">> 5s")) {
            es.videoPlayer.skip(5.0);
        }

        // Progress slider
        float progress = state.getProgress();
        if (ImGui::SliderFloat("Progress", &progress, 0.0f, 1.0f, "%.1f%%")) {
            es.videoPlayer.seekProgress(progress);
        }

        // Time display
        ImGui::Text("Time: %.1f / %.1f s  Frame: %lld / %lld",
                   state.position, state.duration,
                   static_cast<long long>(state.currentFrame),
                   static_cast<long long>(state.totalFrames));

        ImGui::Separator();

        // Speed control
        float speed = es.videoPlayer.getSpeed();
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 4.0f, "%.1fx")) {
            es.videoPlayer.setSpeed(speed);
        }
        ImGui::SameLine();
        if (ImGui::Button("1x")) {
            es.videoPlayer.setSpeed(1.0f);
        }

        // Loop mode
        const char* loopModes[] = { "Once", "Loop", "Ping-Pong" };
        int loopMode = static_cast<int>(es.videoPlayer.getLoopMode());
        if (ImGui::Combo("Loop Mode", &loopMode, loopModes, 3)) {
            es.videoPlayer.setLoopMode(static_cast<LoopMode>(loopMode));
        }

        ImGui::Separator();

        // Assign to quad section
        ImGui::Text("Assign Video to Quad:");
        Quad* selected = es.quadManager.getPrimarySelectedQuad();
        if (selected) {
            bool isAssigned = (es.videoAssignedQuadId == selected->getId());
            if (ImGui::Button(isAssigned ? "Remove from Quad" : "Assign to Selected Quad")) {
                if (isAssigned) {
                    // Remove video texture from quad
                    es.quadTextures.erase(selected->getId());
                    es.videoAssignedQuadId = UUID::null();  // Clear
                } else {
                    // Create a texture wrapper for the video
                    // Note: We'll use the raw OpenGL texture ID
                    es.videoAssignedQuadId = selected->getId();
                }
                ctx.project->setModified(true);
            }
            if (isAssigned) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                                  "Video assigned to: %s", selected->getName().c_str());
            }
        } else {
            ImGui::TextDisabled("Select a quad to assign video");
        }

        // Unload button
        ImGui::Separator();
        if (ImGui::Button("Unload Video")) {
            es.videoPlayer.unload();
            if (es.videoTextureId != 0) {
                glDeleteTextures(1, &es.videoTextureId);
                es.videoTextureId = 0;
                es.videoTextureWidth = 0;
                es.videoTextureHeight = 0;
            }
            es.videoAssignedQuadId = UUID::null();
        }
    } else {
        ImGui::TextDisabled("No video loaded");
        ImGui::TextDisabled("Enter a video file path and click Load");
    }

    ImGui::End();
}

}  // namespace quadmapper
