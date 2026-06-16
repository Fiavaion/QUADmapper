#include "InspectorPanel.hpp"
#include "scene/Quad.hpp"
#include "source/SourceManager.hpp"
#include "source/TextureSource.hpp"
#include "source/VideoFileSource.hpp"
#include "source/procedural/ProceduralSource.hpp"
#include "media/MediaLibrary.hpp"
#include "media/MediaItem.hpp"
#include <imgui.h>
#include <cmath>

namespace quadmapper {

// Helper to format time as mm:ss
static std::string formatTime(double seconds) {
    int mins = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    return buf;
}

InspectorPanel::InspectorPanel() : Panel("Inspector") {}

void InspectorPanel::render() {
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(title_.c_str(), &visible_)) {
        if (selectedQuadId_.isNull()) {
            renderNoSelection();
        }
    }
    ImGui::End();
}

void InspectorPanel::renderNoSelection() {
    ImGui::TextDisabled("No quad selected");
    ImGui::TextDisabled("Select a quad in the scene to view properties");
}

void InspectorPanel::renderQuadProperties(Quad* quad) {
    if (ImGui::CollapsingHeader("Quad Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Name
        char nameBuf[256];
        strncpy(nameBuf, quad->getName().c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            quad->setName(nameBuf);
        }

        // Visibility
        bool visible = quad->isVisible();
        if (ImGui::Checkbox("Visible", &visible)) {
            quad->setVisible(visible);
        }

        // Locked
        ImGui::SameLine();
        bool locked = quad->isLocked();
        if (ImGui::Checkbox("Locked", &locked)) {
            quad->setLocked(locked);
        }

        // Opacity
        float opacity = quad->getOpacity();
        if (ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f)) {
            quad->setOpacity(opacity);
        }

        // Z-Order
        int zOrder = quad->getZOrder();
        if (ImGui::InputInt("Z-Order", &zOrder)) {
            quad->setZOrder(zOrder);
        }

        // UV Transform section
        if (ImGui::TreeNode("UV Transform")) {
            TextureBinding& tex = quad->getTexture();

            float offset[2] = { tex.uvOffset.x, tex.uvOffset.y };
            if (ImGui::DragFloat2("Offset", offset, 0.01f, -2.0f, 2.0f)) {
                tex.uvOffset = Vec2(offset[0], offset[1]);
            }

            float scale[2] = { tex.uvScale.x, tex.uvScale.y };
            if (ImGui::DragFloat2("Scale", scale, 0.01f, 0.1f, 10.0f)) {
                tex.uvScale = Vec2(scale[0], scale[1]);
            }

            float rotDeg = tex.uvRotation * 180.0f / 3.14159265f;
            if (ImGui::SliderFloat("Rotation", &rotDeg, -180.0f, 180.0f, "%.1f deg")) {
                tex.uvRotation = rotDeg * 3.14159265f / 180.0f;
            }

            if (ImGui::Checkbox("Flip H", &tex.flipH)) {}
            ImGui::SameLine();
            if (ImGui::Checkbox("Flip V", &tex.flipV)) {}

            if (ImGui::Button("Reset UV")) {
                tex.resetUV();
            }

            ImGui::TreePop();
        }
    }
}

void InspectorPanel::renderSourceInfo(Quad* quad) {
    if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        const TextureBinding& tex = quad->getTexture();

        if (tex.mediaId.isNull()) {
            ImGui::TextDisabled("No source assigned");
            ImGui::TextDisabled("Drag a media item from the bin");
            return;
        }

        // Get the media item
        if (!mediaLibrary_) {
            ImGui::TextDisabled("Media library not connected");
            return;
        }

        MediaItem* item = mediaLibrary_->getItem(tex.mediaId);
        if (!item) {
            ImGui::TextDisabled("Source not found in library");
            return;
        }

        // Show source name and type
        ImGui::Text("Name: %s", item->getName().c_str());

        switch (item->getType()) {
            case MediaType::IMAGE:
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "[IMAGE]");
                break;
            case MediaType::VIDEO:
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "[VIDEO]");
                break;
            case MediaType::PROCEDURAL:
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[PROCEDURAL]");
                break;
            default:
                ImGui::TextDisabled("[UNKNOWN]");
                break;
        }

        // Get the actual source for controls
        if (!sourceManager_ || !item->hasSource()) {
            return;
        }

        TextureSource* source = sourceManager_->getSource(item->getSourceId());
        if (!source) {
            return;
        }

        ImGui::TextDisabled("Resolution: %d x %d", source->getWidth(), source->getHeight());

        // Type-specific controls
        if (item->getType() == MediaType::VIDEO) {
            renderVideoControls(source);
        } else if (item->getType() == MediaType::PROCEDURAL) {
            renderProceduralInfo(source, item->getId());
        }
    }
}

void InspectorPanel::renderVideoControls(TextureSource* source) {
    auto* playable = dynamic_cast<PlayableSource*>(source);
    if (!playable) {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Playback Controls");

    // Play/Pause/Stop buttons
    PlaybackState state = playable->getPlaybackState();
    bool isPlaying = (state == PlaybackState::PLAYING);

    if (isPlaying) {
        if (ImGui::Button("Pause")) {
            playable->pause();
        }
    } else {
        if (ImGui::Button("Play")) {
            playable->play();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        playable->stop();
    }

    // Progress bar / seek
    double duration = playable->getDuration();
    double currentTime = playable->getCurrentTime();
    float progress = playable->getProgress();

    ImGui::Text("%s / %s", formatTime(currentTime).c_str(), formatTime(duration).c_str());

    if (ImGui::SliderFloat("##seek", &progress, 0.0f, 1.0f, "")) {
        playable->seekProgress(progress);
    }

    // Speed
    float speed = playable->getSpeed();
    if (ImGui::SliderFloat("Speed", &speed, 0.25f, 4.0f, "%.2fx")) {
        playable->setSpeed(speed);
    }

    // Loop mode
    LoopMode loop = playable->getLoopMode();
    const char* loopItems[] = { "None", "Loop", "Ping-Pong" };
    int loopIdx = static_cast<int>(loop);
    if (ImGui::Combo("Loop", &loopIdx, loopItems, 3)) {
        playable->setLoopMode(static_cast<LoopMode>(loopIdx));
    }
}

void InspectorPanel::renderProceduralInfo(TextureSource* source, const UUID& mediaItemId) {
    auto* procedural = dynamic_cast<ProceduralSource*>(source);
    if (!procedural) {
        return;
    }

    ImGui::Separator();

    if (procedural->isAnimated()) {
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Animated");
    } else {
        ImGui::TextDisabled("Static");
    }

    // Button to open editor
    if (ImGui::Button("Edit Parameters...")) {
        if (editProceduralCallback_) {
            editProceduralCallback_(mediaItemId);
        }
    }
}

} // namespace quadmapper
