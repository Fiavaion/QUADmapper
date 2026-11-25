#include "CompositionPanel.hpp"

#include "core/FileSystem.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/MediaAssign.hpp"
#include "editor/PerformLock.hpp"
#include "media/MediaItem.hpp"
#include "media/MediaLibrary.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Scene.hpp"
#include "source/CompositionSource.hpp"
#include "source/SourceManager.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace quadmapper {

namespace {

std::string cueLabel(SourceManager& sm, const CompositionCue& cue) {
    if (cue.sourceId.isNull()) return "(black)";
    const TextureSource* source = sm.getSource(cue.sourceId);
    return source ? source->getName() : std::string("(missing)");
}

// A media bin item as a cue source (deduped, as when it is put on a quad). Null for
// audio and for compositions (no nesting).
UUID cueSourceFor(SourceManager& sm, const MediaItem& item) {
    bool isVideo = false;
    return resolveMediaSource(sm, item, isVideo);
}

}  // namespace

void CompositionPanel::drawContent(EditorContext& ctx) {
    SourceManager* sm = ctx.sourceManager;
    if (!sm) return;
    ImGui::TextWrapped("A composition plays its cues one after another with a crossfade (Timeline), or "
                       "switches between them on Next, Previous or Cue n (Switcher), from here, OSC or MIDI. "
                       "Put it on quads like any video.");
    drawList(ctx);
    CompositionSource* comp = sm->getSourceAs<CompositionSource>(selected_);
    if (!comp) return;
    const bool locked = ctx.es.performLock.isOn();
    ImGui::Separator();
    ImGui::PushID(comp->getId().toString().c_str());
    drawTransport(ctx, *comp);
    ImGui::Separator();
    drawSettings(ctx, *comp, locked);
    ImGui::Separator();
    drawCues(ctx, *comp, locked);
    ImGui::PopID();
}

void CompositionPanel::drawList(EditorContext& ctx) {
    SourceManager& sm = *ctx.sourceManager;
    EditorState& es = ctx.es;
    const bool locked = es.performLock.isOn();
    const std::vector<UUID>& ids = sm.getCompositionIds();
    if (!sm.getSourceAs<CompositionSource>(selected_)) selected_ = ids.empty() ? UUID() : ids.front();

    ImGui::BeginDisabled(locked);
    auto create = [&](CompositionMode mode) {
        CompositionSettings settings;
        settings.mode = mode;
        const int number = static_cast<int>(ids.size()) + 1;
        settings.name = (mode == CompositionMode::Timeline ? "Timeline " : "Switcher ") + std::to_string(number);
        selected_ = sm.createCompositionSource(std::move(settings));
    };
    if (ImGui::Button("New Timeline")) create(CompositionMode::Timeline);
    ImGui::SetItemTooltip("Cues play one after another, each for its hold time, with a crossfade");
    ImGui::SameLine();
    if (ImGui::Button("New Switcher")) create(CompositionMode::Switch);
    ImGui::SetItemTooltip("Cues change only on Next, Previous or Cue n (here, OSC or MIDI)");
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_.isNull());
    if (ImGui::Button("Delete")) {
        size_t quadCount = 0, objectCount = 0;
        for (const auto& scene : es.scenes) {
            if (!scene) continue;
            for (const auto& quad : scene->getQuadManager().getQuads()) {
                if (quad->getTexture().mediaId == selected_) ++quadCount;
            }
            for (const auto& object : scene->getObjects()) {
                if (object && object->getTexture().mediaId == selected_) ++objectCount;
            }
        }
        const auto* target = sm.getSourceAs<CompositionSource>(selected_);
        std::string message = "Delete composition '" + (target ? target->getName() : std::string()) + "'?";
        if (quadCount + objectCount > 0) {
            message += " " + std::to_string(quadCount) + (quadCount == 1 ? " quad" : " quads") + " and " +
                       std::to_string(objectCount) + (objectCount == 1 ? " 3D object" : " 3D objects") +
                       " showing it will show nothing.";
        }
        pendingDeleteId_ = selected_;
        deleteConfirm_.request("Delete Composition?", message + " This cannot be undone.", "Delete");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // The confirmation may be answered after the show was locked.
    if (deleteConfirm_.draw() && !refusedByPerformLock(es, "Delete Composition")) {
        const UUID gone = pendingDeleteId_;
        pendingDeleteId_ = UUID::null();
        // Quads and objects showing it show nothing (black) until given other media.
        for (const auto& scene : es.scenes) {
            if (!scene) continue;
            for (const auto& quad : scene->getQuadManager().getQuads()) {
                if (quad->getTexture().mediaId == gone) quad->getTexture().mediaId = UUID();
            }
            for (const auto& object : scene->getObjects()) {
                if (object && object->getTexture().mediaId == gone) object->getTexture().mediaId = UUID();
            }
        }
        // The clips it paused run again, for anything else showing them.
        if (const auto* comp = sm.getSourceAs<CompositionSource>(gone); comp && comp->settings().restartOnCue) {
            for (const CompositionCue& cue : comp->settings().cues) {
                if (auto* clip = sm.getSourceAs<PlayableSource>(cue.sourceId)) clip->play();
            }
        }
        sm.removeSource(gone);
        if (selected_ == gone) selected_ = UUID();
    }

    const std::vector<UUID>& list = sm.getCompositionIds();
    if (list.empty()) {
        ImGui::TextDisabled("No compositions yet.");
        return;
    }
    if (ImGui::BeginListBox("##compositions", ImVec2(-1.0f, ImGui::GetTextLineHeightWithSpacing() * 4.5f))) {
        for (size_t i = 0; i < list.size(); ++i) {
            const auto* c = sm.getSourceAs<CompositionSource>(list[i]);
            if (!c) continue;
            char label[160];
            std::snprintf(label, sizeof(label), "%s  (%s, %zu cues)##%s", c->getName().c_str(),
                          c->settings().mode == CompositionMode::Timeline ? "Timeline" : "Switcher",
                          c->settings().cues.size(), list[i].toString().c_str());
            if (ImGui::Selectable(label, list[i] == selected_)) selected_ = list[i];
            ImGui::SetItemTooltip("OSC: /qm/composition/%s/next, /prev, /cue i <n>, /play, /pause, /restart, "
                                  "/fade f <seconds> (the name, or its id)",
                                  c->getName().c_str());
        }
        ImGui::EndListBox();
    }
}

void CompositionPanel::drawTransport(EditorContext& ctx, CompositionSource& comp) {
    (void)ctx;
    const CompositionSettings& s = comp.settings();
    CompositionPlayhead& p = comp.playhead();
    const int n = static_cast<int>(s.cues.size());
    if (n == 0) {
        ImGui::TextDisabled("Add cues below.");
        return;
    }
    if (ImGui::Button("Previous")) p.prev(s);
    ImGui::SameLine();
    if (ImGui::Button("Next")) p.next(s);
    if (s.mode == CompositionMode::Timeline) {
        ImGui::SameLine();
        if (p.running()) {
            if (ImGui::Button("Pause")) p.pause();
        } else if (ImGui::Button("Play")) {
            p.play();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) comp.restart();
    // One button per cue: the current one highlighted.
    for (int i = 0; i < n; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool on = i == p.current();
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        char label[16];
        std::snprintf(label, sizeof(label), "%d", i + 1);
        if (ImGui::Button(label, ImVec2(ImGui::GetFrameHeight() * 1.4f, 0.0f))) p.cue(i, s);
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Cue %d: %s", i + 1, cueLabel(*ctx.sourceManager, s.cues[static_cast<size_t>(i)]).c_str());
    }
    const double hold = s.cues[static_cast<size_t>(std::clamp(p.current(), 0, n - 1))].holdSeconds;
    if (s.mode == CompositionMode::Timeline && hold > 0.0) {
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "Cue %d  %.1f / %.1f s", p.current() + 1, p.timeInCue(), hold);
        ImGui::ProgressBar(static_cast<float>(std::min(1.0, p.timeInCue() / hold)), ImVec2(-1.0f, 0.0f), overlay);
    } else {
        ImGui::Text("Cue %d of %d", p.current() + 1, n);
    }
}

void CompositionPanel::drawSettings(EditorContext& ctx, CompositionSource& comp, bool locked) {
    EditorState& es = ctx.es;
    CompositionSettings& s = comp.editSettings();
    ImGui::BeginDisabled(locked);
    if (nameFor_ != comp.getId()) {
        nameFor_ = comp.getId();
        std::snprintf(nameEdit_, sizeof(nameEdit_), "%s", s.name.c_str());
    }
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText("Name", nameEdit_, sizeof(nameEdit_)) && nameEdit_[0] != '\0') s.name = nameEdit_;

    int mode = s.mode == CompositionMode::Timeline ? 0 : 1;
    if (ImGui::RadioButton("Timeline", mode == 0)) s.mode = CompositionMode::Timeline;
    ImGui::SameLine();
    if (ImGui::RadioButton("Switcher", mode == 1)) s.mode = CompositionMode::Switch;

    float fade = static_cast<float>(s.fadeSeconds);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat("Crossfade", &fade, 0.05f, 0.0f, 30.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp)) {
        s.fadeSeconds = fade;
    }
    ImGui::SetItemTooltip("0 = a cut");
    if (s.mode == CompositionMode::Timeline) {
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &s.loop);
    }
    ImGui::Checkbox("Restart clips on cue", &s.restartOnCue);
    ImGui::SetItemTooltip("On: a video starts from its beginning when its cue comes on and pauses once "
                          "faded out. Off: every clip keeps running and a switch only changes what shows.");

    int size[2] = {s.width, s.height};
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt2("Size (pixels)", size, ImGuiInputTextFlags_EnterReturnsTrue)) {
        s.width = std::clamp(size[0], 16, 8192);
        s.height = std::clamp(size[1], 16, 8192);
    }

    // Put it on the selected quads of the active scene.
    std::vector<Quad*> selected = es.activeScene().getQuadManager().getSelectedQuads();
    ImGui::BeginDisabled(selected.empty());
    char label[64];
    std::snprintf(label, sizeof(label), "Put on %zu selected quad%s", selected.size(), selected.size() == 1 ? "" : "s");
    if (ImGui::Button(selected.empty() ? "Put on the selected quads" : label)) {
        for (Quad* quad : selected) bindSourceToQuad(es, *quad, comp.getId(), std::string(), false);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void CompositionPanel::drawCues(EditorContext& ctx, CompositionSource& comp, bool locked) {
    SourceManager& sm = *ctx.sourceManager;
    CompositionSettings& s = comp.editSettings();
    ImGui::TextUnformatted("Cues");
    ImGui::BeginDisabled(locked);
    int removeAt = -1, moveUp = -1, moveDown = -1;
    if (ImGui::BeginTable("##cues", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Hold", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.cues.size(); ++i) {
            CompositionCue& cue = s.cues[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cueLabel(sm, cue).c_str());
            ImGui::TableNextColumn();
            if (s.mode == CompositionMode::Timeline) {
                float hold = static_cast<float>(cue.holdSeconds);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat("##hold", &hold, 0.1f, 0.0f, 3600.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp)) {
                    cue.holdSeconds = hold;
                    cue.holdFromClip = false;
                }
                ImGui::SetItemTooltip("How long it shows before the next cue; 0 = wait for Next");
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveUp = static_cast<int>(i);
            ImGui::SetItemTooltip("Move this cue up");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == s.cues.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) moveDown = static_cast<int>(i);
            ImGui::SetItemTooltip("Move this cue down");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
            ImGui::SetItemTooltip("Remove this cue");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (moveUp > 0) std::swap(s.cues[static_cast<size_t>(moveUp)], s.cues[static_cast<size_t>(moveUp - 1)]);
    if (moveDown >= 0) std::swap(s.cues[static_cast<size_t>(moveDown)], s.cues[static_cast<size_t>(moveDown + 1)]);
    if (removeAt >= 0) s.cues.erase(s.cues.begin() + removeAt);

    // Add a cue: from the media bin list, or by dropping a bin item here.
    auto addItem = [&](const MediaItem& item) {
        const UUID id = cueSourceFor(sm, item);
        if (id.isNull() || sm.getSourceAs<CompositionSource>(id)) return;
        CompositionCue cue;
        cue.sourceId = id;
        if (const auto* clip = sm.getSourceAs<PlayableSource>(id)) {
            // A video holds for its length, once its file has opened.
            if (clip->getDuration() > 0.0) cue.holdSeconds = clip->getDuration();
            else cue.holdFromClip = true;
        }
        s.cues.push_back(cue);
    };
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("##addcue", "Add cue from the media bin...")) {
        if (ctx.mediaLibrary) {
            for (const auto& item : ctx.mediaLibrary->getItems()) {
                if (!item || item->getType() == MediaType::AUDIO) continue;
                if (ImGui::Selectable(item->getName().c_str(), false)) addItem(*item);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM_ID")) {
            const UUID itemId = *static_cast<const UUID*>(payload->Data);
            if (ctx.mediaLibrary) {
                if (const MediaItem* item = ctx.mediaLibrary->getItem(itemId)) addItem(*item);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(or drop media here)");
    ImGui::EndDisabled();
}

}  // namespace quadmapper
