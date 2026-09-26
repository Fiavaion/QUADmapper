#include "ControlPanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "control/ExternalControl.hpp"
#include "output/OutputManager.hpp"
#include "project/Project.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace quadmapper {

namespace {

using control::ControlRegistry;
using control::MidiControlType;
using control::MidiMapping;
using control::MidiMode;
using control::TargetKind;

constexpr double kActivityLitSeconds = 0.3;
const ImVec4 kGood(0.35f, 0.85f, 0.45f, 1.0f);
const ImVec4 kBad(1.0f, 0.35f, 0.3f, 1.0f);
const ImVec4 kLearn(1.0f, 0.8f, 0.2f, 1.0f);

// A small light that is on for a moment after `lastActivity` (seconds, < 0 = never).
void activityLight(double lastActivity, double now) {
    const float lineHeight = ImGui::GetTextLineHeight();
    const float radius = lineHeight * 0.3f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool lit = lastActivity >= 0.0 && now - lastActivity < kActivityLitSeconds;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(origin.x + radius + 2.0f, origin.y + lineHeight * 0.5f), radius,
        lit ? ImGui::GetColorU32(kGood) : ImGui::GetColorU32(ImGuiCol_FrameBg));
    ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, lineHeight));
    ImGui::SameLine();
}

bool containsIgnoreCase(const std::string& text, const char* needle) {
    if (!needle[0]) return true;
    std::string a = text;
    std::string b = needle;
    auto lower = [](std::string& s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    lower(a);
    lower(b);
    return a.find(b) != std::string::npos;
}

std::string kindLabel(TargetKind kind, float min, float max) {
    char text[48];
    switch (kind) {
        case TargetKind::Trigger: return "trigger";
        case TargetKind::Bool:    return "on/off";
        case TargetKind::Index:   return "number";
        case TargetKind::Float:
            std::snprintf(text, sizeof(text), "%g-%g", static_cast<double>(min), static_cast<double>(max));
            return text;
    }
    return "";
}

std::string controlLabel(const MidiMapping& m) {
    char text[64];
    const char* type = m.type == MidiControlType::Note ? "Note" : m.type == MidiControlType::CC ? "CC" : "Program";
    if (m.channel == 0) std::snprintf(text, sizeof(text), "%s %d, any ch", type, m.number);
    else                std::snprintf(text, sizeof(text), "%s %d, ch %d", type, m.number, m.channel);
    return text;
}

}  // namespace

bool ControlPanel::render(EditorContext& ctx, control::ExternalControl& control,
                          ControlPreferences& prefs, double now) {
    EditorState& es = ctx.es;
    if (!es.showControl) return false;
    if (!editsLoaded_) {
        portEdit_ = prefs.oscPort;
        std::snprintf(bindEdit_, sizeof(bindEdit_), "%s", prefs.oscBindAddress.c_str());
        editsLoaded_ = true;
    }

    bool changed = false;
    if (!ImGui::Begin("Control", &es.showControl)) {
        ImGui::End();
        return false;
    }

    if (ctx.outputManager) {
        const bool on = ctx.outputManager->isBlackout();
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.15f, 0.15f, 1.0f));
        }
        if (ImGui::Button(on ? "BLACKOUT ON - click to restore (Ctrl+B)" : "Blackout (Ctrl+B)")) {
            ctx.outputManager->toggleBlackout();
        }
        if (on) ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Every projector output goes black at once (also /qm/blackout)");
    }

    if (ImGui::CollapsingHeader("OSC", ImGuiTreeNodeFlags_DefaultOpen)) changed |= renderOsc(control, prefs);
    if (ImGui::CollapsingHeader("MIDI", ImGuiTreeNodeFlags_DefaultOpen)) changed |= renderMidi(control, prefs, now);
    if (ImGui::CollapsingHeader("MIDI Learn", ImGuiTreeNodeFlags_DefaultOpen)) renderTargets(control, now);
    if (ImGui::CollapsingHeader("MIDI Mappings", ImGuiTreeNodeFlags_DefaultOpen)) renderMappings(ctx, control, now);

    ImGui::End();
    return changed;
}

bool ControlPanel::renderOsc(control::ExternalControl& control, ControlPreferences& prefs) {
    bool changed = false;
    const control::OscServer& osc = control.osc();

    bool enabled = prefs.oscEnabled;
    if (ImGui::Checkbox("Enable OSC input", &enabled)) {
        prefs.oscEnabled = enabled;
        changed = true;
        if (enabled) control.startOsc(prefs.oscBindAddress, prefs.oscPort);
        else         control.stopOsc();
    }

    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("UDP port", &portEdit_, 0, 0);
    portEdit_ = std::clamp(portEdit_, 1, 65535);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("Bind address", bindEdit_, sizeof(bindEdit_));
    ImGui::SetItemTooltip("0.0.0.0 = every network interface; 127.0.0.1 = this computer only");
    const bool edited = portEdit_ != prefs.oscPort || prefs.oscBindAddress != bindEdit_;
    ImGui::BeginDisabled(!edited);
    if (ImGui::Button("Apply")) {
        prefs.oscPort = portEdit_;
        prefs.oscBindAddress = bindEdit_;
        changed = true;
        if (prefs.oscEnabled) control.startOsc(prefs.oscBindAddress, prefs.oscPort);
    }
    ImGui::EndDisabled();
    if (edited) {
        ImGui::SameLine();
        ImGui::TextDisabled("(not applied)");
    }

    if (osc.isRunning()) {
        ImGui::TextColored(kGood, "Listening on UDP %s:%d", prefs.oscBindAddress.c_str(), osc.boundPort());
    } else if (!control.oscError().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kBad);
        ImGui::TextWrapped("Error: %s", control.oscError().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Off");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Enabling OSC opens a UDP port. Windows Firewall asks the first time: allow "
                       "private networks for a controller on your network.");
    ImGui::PopStyleColor();

    const control::ControlDispatcher& d = control.dispatcher();
    ImGui::Text("Packets %llu   malformed %llu   messages %llu",
                static_cast<unsigned long long>(osc.packetsReceived()),
                static_cast<unsigned long long>(osc.malformedPackets()),
                static_cast<unsigned long long>(osc.messagesReceived()));
    ImGui::Text("Unknown addresses %llu   bad arguments %llu   dropped %llu",
                static_cast<unsigned long long>(d.unknownAddresses()),
                static_cast<unsigned long long>(d.badArguments()),
                static_cast<unsigned long long>(control.queue().droppedCount()));

    if (ImGui::TreeNodeEx("Last messages", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (d.recentOsc().empty()) ImGui::TextDisabled("None yet");
        for (const auto& line : d.recentOsc()) ImGui::TextUnformatted(line.c_str());
        ImGui::TreePop();
    }
    if (!d.recentUnknown().empty() && ImGui::TreeNode("Unknown addresses")) {
        for (const auto& address : d.recentUnknown()) ImGui::TextColored(kBad, "%s", address.c_str());
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Address reference")) {
        ImGui::BulletText("/qm/blackout  [0|1, none = toggle]");
        ImGui::BulletText("/qm/scene/take n  (n = 1, 2, ...)");
        ImGui::BulletText("/qm/scene/<id or name>/take");
        ImGui::BulletText("/qm/quad/<id or name>/opacity 0..1");
        ImGui::BulletText("/qm/quad/<id or name>/visible 0|1");
        ImGui::BulletText("/qm/source/<id or name>/play | pause | restart");
        ImGui::BulletText("/qm/source/<id or name>/speed 0.1..4");
        ImGui::TextDisabled("Names match without case; a shared name uses the first match.");
        ImGui::TreePop();
    }
    return changed;
}

bool ControlPanel::renderMidi(control::ExternalControl& control, ControlPreferences& prefs, double now) {
    bool changed = false;
    if (ImGui::Button("Refresh")) control.rescanMidi();
    ImGui::SameLine();
    const auto& available = control.availableMidiDevices();
    ImGui::TextDisabled("%zu MIDI input device(s)", available.size());

    std::vector<std::string> names = available;
    for (const auto& wanted : prefs.midiDevices) {
        if (std::find(names.begin(), names.end(), wanted) == names.end()) names.push_back(wanted);
    }
    if (names.empty()) {
        ImGui::TextDisabled("No MIDI input devices found. Connect one and press Refresh.");
    }
    for (const auto& name : names) {
        ImGui::PushID(name.c_str());
        auto it = std::find(prefs.midiDevices.begin(), prefs.midiDevices.end(), name);
        bool wanted = it != prefs.midiDevices.end();
        if (ImGui::Checkbox(name.c_str(), &wanted)) {
            if (wanted) prefs.midiDevices.push_back(name);
            else        prefs.midiDevices.erase(it);
            control.setWantedMidiDevices(prefs.midiDevices);
            changed = true;
        }
        ImGui::SameLine();
        if (control.midi().isOpen(name)) {
            ImGui::TextColored(kGood, "open");
        } else if (wanted) {
            const std::string status = control.midiDeviceStatus(name);
            ImGui::TextColored(kBad, "%s", status.empty() ? "not open" : status.c_str());
        } else {
            ImGui::TextDisabled("off");
        }
        ImGui::PopID();
    }

    const control::ControlDispatcher& d = control.dispatcher();
    activityLight(d.lastMidiTime(), now);
    if (d.lastMidi().empty()) ImGui::TextDisabled("No MIDI received yet");
    else                      ImGui::Text("Last: %s", d.lastMidi().c_str());
    return changed;
}

void ControlPanel::renderTargets(control::ExternalControl& control, double now) {
    control::ControlDispatcher& d = control.dispatcher();
    ControlRegistry& registry = control.registry();

    if (d.isLearning()) {
        const std::string label = registry.describe(d.learnTarget()).value_or(d.learnTarget());
        ImGui::PushStyleColor(ImGuiCol_Text, kLearn);
        ImGui::TextWrapped("Move a control on a MIDI device to bind it to %s. Esc cancels.", label.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("Cancel learn")) d.cancelLearn();
    } else {
        ImGui::TextDisabled("Click Learn, then press a pad/key or move a knob/fader.");
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##targetFilter", "Filter targets", targetFilter_, sizeof(targetFilter_));

    const auto targets = registry.listTargets();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float height = ImGui::GetTextLineHeightWithSpacing() * 10.0f;
    if (!ImGui::BeginTable("controlTargets", 3, flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("##learn", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableHeadersRow();
    for (const auto& target : targets) {
        if (!containsIgnoreCase(target.label, targetFilter_)) continue;
        ImGui::PushID(target.path.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        activityLight(d.lastActivity(target.path), now);
        ImGui::TextUnformatted(target.label.c_str());
        ImGui::SetItemTooltip("%s", target.path.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", kindLabel(target.kind, target.min, target.max).c_str());
        ImGui::TableNextColumn();
        const bool learningThis = d.isLearning() && d.learnTarget() == target.path;
        if (learningThis) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.45f, 0.05f, 1.0f));
            if (ImGui::SmallButton("Waiting")) d.cancelLearn();
            ImGui::PopStyleColor();
        } else if (ImGui::SmallButton("Learn")) {
            d.beginLearn(target.path);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void ControlPanel::renderMappings(EditorContext& ctx, control::ExternalControl& control, double now) {
    auto& mappings = ctx.es.midiMappings;
    ControlRegistry& registry = control.registry();
    if (mappings.empty()) {
        ImGui::TextDisabled("No mappings yet. Click Learn on a target, then move a control.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("midiMappings", 5, flags)) return;
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("##delete", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableHeadersRow();

    static const char* kModes[] = {"Trigger", "Toggle", "Momentary", "Value"};
    int removeIndex = -1;
    for (size_t i = 0; i < mappings.size(); ++i) {
        MidiMapping& m = mappings[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        activityLight(m.lastActivity, now);
        ImGui::TextUnformatted(controlLabel(m).c_str());
        if (m.lastValue >= 0) ImGui::SetItemTooltip("Last value %d", m.lastValue);

        ImGui::TableNextColumn();
        if (auto label = registry.describe(m.target)) {
            ImGui::TextUnformatted(label->c_str());
            ImGui::SetItemTooltip("%s", m.target.c_str());
        } else {
            ImGui::TextColored(kBad, "Unresolved");
            ImGui::SetItemTooltip("%s\nThe quad, scene or source this mapping drove is gone. "
                                  "The mapping is kept (undo may bring it back); delete it if not.",
                                  m.target.c_str());
        }

        ImGui::TableNextColumn();
        int mode = static_cast<int>(m.mode);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##mode", &mode, kModes, IM_ARRAYSIZE(kModes))) {
            m.mode = static_cast<MidiMode>(mode);
            m.ccHigh = false;
            if (ctx.project) ctx.project->setModified(true);
        }

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(m.device.c_str());
        if (!ControlRegistry::equalsIgnoreCase(m.device, "any")) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Any")) {
                m.device = "any";
                if (ctx.project) ctx.project->setModified(true);
            }
            ImGui::SetItemTooltip("Accept this control from any MIDI device");
        }

        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Delete")) removeIndex = static_cast<int>(i);
        ImGui::PopID();
    }
    ImGui::EndTable();

    if (removeIndex >= 0) {
        mappings.erase(mappings.begin() + removeIndex);
        if (ctx.project) ctx.project->setModified(true);
    }
}

}  // namespace quadmapper
