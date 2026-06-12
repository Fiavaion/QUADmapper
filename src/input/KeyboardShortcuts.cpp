#include "KeyboardShortcuts.hpp"
#include "core/Logger.hpp"
#include "core/FileSystem.hpp"

#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

namespace quadmapper {

// Static key name mappings
static const std::unordered_map<int, std::string> KEY_NAMES = {
    {GLFW_KEY_SPACE, "Space"},
    {GLFW_KEY_APOSTROPHE, "'"},
    {GLFW_KEY_COMMA, ","},
    {GLFW_KEY_MINUS, "-"},
    {GLFW_KEY_PERIOD, "."},
    {GLFW_KEY_SLASH, "/"},
    {GLFW_KEY_0, "0"}, {GLFW_KEY_1, "1"}, {GLFW_KEY_2, "2"}, {GLFW_KEY_3, "3"},
    {GLFW_KEY_4, "4"}, {GLFW_KEY_5, "5"}, {GLFW_KEY_6, "6"}, {GLFW_KEY_7, "7"},
    {GLFW_KEY_8, "8"}, {GLFW_KEY_9, "9"},
    {GLFW_KEY_SEMICOLON, ";"},
    {GLFW_KEY_EQUAL, "="},
    {GLFW_KEY_A, "A"}, {GLFW_KEY_B, "B"}, {GLFW_KEY_C, "C"}, {GLFW_KEY_D, "D"},
    {GLFW_KEY_E, "E"}, {GLFW_KEY_F, "F"}, {GLFW_KEY_G, "G"}, {GLFW_KEY_H, "H"},
    {GLFW_KEY_I, "I"}, {GLFW_KEY_J, "J"}, {GLFW_KEY_K, "K"}, {GLFW_KEY_L, "L"},
    {GLFW_KEY_M, "M"}, {GLFW_KEY_N, "N"}, {GLFW_KEY_O, "O"}, {GLFW_KEY_P, "P"},
    {GLFW_KEY_Q, "Q"}, {GLFW_KEY_R, "R"}, {GLFW_KEY_S, "S"}, {GLFW_KEY_T, "T"},
    {GLFW_KEY_U, "U"}, {GLFW_KEY_V, "V"}, {GLFW_KEY_W, "W"}, {GLFW_KEY_X, "X"},
    {GLFW_KEY_Y, "Y"}, {GLFW_KEY_Z, "Z"},
    {GLFW_KEY_LEFT_BRACKET, "["},
    {GLFW_KEY_BACKSLASH, "\\"},
    {GLFW_KEY_RIGHT_BRACKET, "]"},
    {GLFW_KEY_GRAVE_ACCENT, "`"},
    {GLFW_KEY_ESCAPE, "Escape"},
    {GLFW_KEY_ENTER, "Enter"},
    {GLFW_KEY_TAB, "Tab"},
    {GLFW_KEY_BACKSPACE, "Backspace"},
    {GLFW_KEY_INSERT, "Insert"},
    {GLFW_KEY_DELETE, "Delete"},
    {GLFW_KEY_RIGHT, "Right"},
    {GLFW_KEY_LEFT, "Left"},
    {GLFW_KEY_DOWN, "Down"},
    {GLFW_KEY_UP, "Up"},
    {GLFW_KEY_PAGE_UP, "PageUp"},
    {GLFW_KEY_PAGE_DOWN, "PageDown"},
    {GLFW_KEY_HOME, "Home"},
    {GLFW_KEY_END, "End"},
    {GLFW_KEY_CAPS_LOCK, "CapsLock"},
    {GLFW_KEY_SCROLL_LOCK, "ScrollLock"},
    {GLFW_KEY_NUM_LOCK, "NumLock"},
    {GLFW_KEY_PRINT_SCREEN, "PrintScreen"},
    {GLFW_KEY_PAUSE, "Pause"},
    {GLFW_KEY_F1, "F1"}, {GLFW_KEY_F2, "F2"}, {GLFW_KEY_F3, "F3"}, {GLFW_KEY_F4, "F4"},
    {GLFW_KEY_F5, "F5"}, {GLFW_KEY_F6, "F6"}, {GLFW_KEY_F7, "F7"}, {GLFW_KEY_F8, "F8"},
    {GLFW_KEY_F9, "F9"}, {GLFW_KEY_F10, "F10"}, {GLFW_KEY_F11, "F11"}, {GLFW_KEY_F12, "F12"},
    {GLFW_KEY_KP_0, "Num0"}, {GLFW_KEY_KP_1, "Num1"}, {GLFW_KEY_KP_2, "Num2"},
    {GLFW_KEY_KP_3, "Num3"}, {GLFW_KEY_KP_4, "Num4"}, {GLFW_KEY_KP_5, "Num5"},
    {GLFW_KEY_KP_6, "Num6"}, {GLFW_KEY_KP_7, "Num7"}, {GLFW_KEY_KP_8, "Num8"},
    {GLFW_KEY_KP_9, "Num9"},
    {GLFW_KEY_KP_DECIMAL, "NumDecimal"},
    {GLFW_KEY_KP_DIVIDE, "NumDivide"},
    {GLFW_KEY_KP_MULTIPLY, "NumMultiply"},
    {GLFW_KEY_KP_SUBTRACT, "NumSubtract"},
    {GLFW_KEY_KP_ADD, "NumAdd"},
    {GLFW_KEY_KP_ENTER, "NumEnter"},
    {GLFW_KEY_KP_EQUAL, "NumEqual"},
};

// Reverse mapping for parsing
static std::unordered_map<std::string, int> buildKeyCodeMap() {
    std::unordered_map<std::string, int> map;
    for (const auto& [code, name] : KEY_NAMES) {
        map[name] = code;
        // Also add lowercase version
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        map[lower] = code;
    }
    return map;
}

static const std::unordered_map<std::string, int> KEY_CODES = buildKeyCodeMap();

std::string KeyCombination::toString() const {
    if (!isValid()) {
        return "None";
    }

    std::string result;

    // Add modifiers in consistent order
    if (hasFlag(modifiers, ModifierFlags::Ctrl)) {
        result += "Ctrl+";
    }
    if (hasFlag(modifiers, ModifierFlags::Shift)) {
        result += "Shift+";
    }
    if (hasFlag(modifiers, ModifierFlags::Alt)) {
        result += "Alt+";
    }
    if (hasFlag(modifiers, ModifierFlags::Super)) {
        result += "Super+";
    }

    // Add key name
    result += KeyboardShortcuts::getKeyName(key);

    return result;
}

KeyCombination KeyCombination::fromString(const std::string& str) {
    if (str.empty() || str == "None") {
        return KeyCombination();
    }

    KeyCombination result;
    std::string remaining = str;

    // Parse modifiers
    while (true) {
        if (remaining.find("Ctrl+") == 0) {
            result.modifiers = result.modifiers | ModifierFlags::Ctrl;
            remaining = remaining.substr(5);
        } else if (remaining.find("Shift+") == 0) {
            result.modifiers = result.modifiers | ModifierFlags::Shift;
            remaining = remaining.substr(6);
        } else if (remaining.find("Alt+") == 0) {
            result.modifiers = result.modifiers | ModifierFlags::Alt;
            remaining = remaining.substr(4);
        } else if (remaining.find("Super+") == 0) {
            result.modifiers = result.modifiers | ModifierFlags::Super;
            remaining = remaining.substr(6);
        } else {
            break;
        }
    }

    // Parse key
    result.key = KeyboardShortcuts::parseKeyName(remaining);

    return result;
}

KeyboardShortcuts::KeyboardShortcuts() {
    // Reserve space for all actions
    shortcuts_.reserve(static_cast<size_t>(ShortcutAction::COUNT));
    initializeDefaults();
}

void KeyboardShortcuts::initializeDefaults() {
    shortcuts_.clear();
    keyToAction_.clear();

    // File operations
    registerShortcut(ShortcutAction::NewProject, "New Project", "File",
                     KeyCombination(GLFW_KEY_N, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::OpenProject, "Open Project", "File",
                     KeyCombination(GLFW_KEY_O, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::SaveProject, "Save Project", "File",
                     KeyCombination(GLFW_KEY_S, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::SaveProjectAs, "Save Project As", "File",
                     KeyCombination(GLFW_KEY_S, ModifierFlags::Ctrl | ModifierFlags::Shift));

    // Edit operations
    registerShortcut(ShortcutAction::Undo, "Undo", "Edit",
                     KeyCombination(GLFW_KEY_Z, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::Redo, "Redo", "Edit",
                     KeyCombination(GLFW_KEY_Y, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::Cut, "Cut", "Edit",
                     KeyCombination(GLFW_KEY_X, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::Copy, "Copy", "Edit",
                     KeyCombination(GLFW_KEY_C, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::Paste, "Paste", "Edit",
                     KeyCombination(GLFW_KEY_V, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::Delete, "Delete", "Edit",
                     KeyCombination(GLFW_KEY_DELETE));
    registerShortcut(ShortcutAction::SelectAll, "Select All", "Edit",
                     KeyCombination(GLFW_KEY_A, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::DeselectAll, "Deselect All", "Edit",
                     KeyCombination(GLFW_KEY_ESCAPE));
    registerShortcut(ShortcutAction::Duplicate, "Duplicate", "Edit",
                     KeyCombination(GLFW_KEY_D, ModifierFlags::Ctrl));

    // View operations
    registerShortcut(ShortcutAction::ToggleFullscreen, "Toggle Fullscreen", "View",
                     KeyCombination(GLFW_KEY_F11));
    registerShortcut(ShortcutAction::ZoomIn, "Zoom In", "View",
                     KeyCombination(GLFW_KEY_EQUAL, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::ZoomOut, "Zoom Out", "View",
                     KeyCombination(GLFW_KEY_MINUS, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::ZoomReset, "Reset Zoom", "View",
                     KeyCombination(GLFW_KEY_0, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::FitToWindow, "Fit to Window", "View",
                     KeyCombination(GLFW_KEY_F, ModifierFlags::Ctrl));

    // Quad operations
    registerShortcut(ShortcutAction::AddQuad, "Add Quad", "Quad",
                     KeyCombination(GLFW_KEY_Q, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::GroupQuads, "Group Quads", "Quad",
                     KeyCombination(GLFW_KEY_G, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::UngroupQuads, "Ungroup Quads", "Quad",
                     KeyCombination(GLFW_KEY_G, ModifierFlags::Ctrl | ModifierFlags::Shift));
    registerShortcut(ShortcutAction::LockQuad, "Lock Quad", "Quad",
                     KeyCombination(GLFW_KEY_L, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::UnlockQuad, "Unlock Quad", "Quad",
                     KeyCombination(GLFW_KEY_L, ModifierFlags::Ctrl | ModifierFlags::Shift));
    registerShortcut(ShortcutAction::HideQuad, "Hide Quad", "Quad",
                     KeyCombination(GLFW_KEY_H, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::ShowQuad, "Show Quad", "Quad",
                     KeyCombination(GLFW_KEY_H, ModifierFlags::Ctrl | ModifierFlags::Shift));

    // Z-order operations (Resolume-style using brackets)
    registerShortcut(ShortcutAction::BringToFront, "Bring to Front", "Arrange",
                     KeyCombination(GLFW_KEY_RIGHT_BRACKET, ModifierFlags::Ctrl | ModifierFlags::Shift));
    registerShortcut(ShortcutAction::BringForward, "Bring Forward", "Arrange",
                     KeyCombination(GLFW_KEY_RIGHT_BRACKET, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::SendBackward, "Send Backward", "Arrange",
                     KeyCombination(GLFW_KEY_LEFT_BRACKET, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::SendToBack, "Send to Back", "Arrange",
                     KeyCombination(GLFW_KEY_LEFT_BRACKET, ModifierFlags::Ctrl | ModifierFlags::Shift));

    // Playback operations (Resolume-style)
    registerShortcut(ShortcutAction::PlayPause, "Play/Pause", "Playback",
                     KeyCombination(GLFW_KEY_SPACE));
    registerShortcut(ShortcutAction::Stop, "Stop", "Playback",
                     KeyCombination(GLFW_KEY_PERIOD));
    registerShortcut(ShortcutAction::NextFrame, "Next Frame", "Playback",
                     KeyCombination(GLFW_KEY_RIGHT));
    registerShortcut(ShortcutAction::PreviousFrame, "Previous Frame", "Playback",
                     KeyCombination(GLFW_KEY_LEFT));
    registerShortcut(ShortcutAction::SkipForward, "Skip Forward 5s", "Playback",
                     KeyCombination(GLFW_KEY_RIGHT, ModifierFlags::Shift));
    registerShortcut(ShortcutAction::SkipBackward, "Skip Backward 5s", "Playback",
                     KeyCombination(GLFW_KEY_LEFT, ModifierFlags::Shift));
    registerShortcut(ShortcutAction::LoopToggle, "Toggle Loop", "Playback",
                     KeyCombination(GLFW_KEY_L));

    // Output operations
    registerShortcut(ShortcutAction::ToggleOutput, "Toggle Output", "Output",
                     KeyCombination(GLFW_KEY_O, ModifierFlags::Ctrl | ModifierFlags::Shift));
    registerShortcut(ShortcutAction::TogglePerformMode, "Toggle Perform Mode", "Output",
                     KeyCombination(GLFW_KEY_P, ModifierFlags::Ctrl | ModifierFlags::Shift));
    registerShortcut(ShortcutAction::BlackoutOutput, "Blackout Output", "Output",
                     KeyCombination(GLFW_KEY_B, ModifierFlags::Ctrl | ModifierFlags::Shift));

    // Mesh operations
    registerShortcut(ShortcutAction::ToggleMeshEdit, "Toggle Mesh Edit", "Mesh",
                     KeyCombination(GLFW_KEY_M, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::IncreaseMeshResolution, "Increase Mesh Resolution", "Mesh",
                     KeyCombination(GLFW_KEY_EQUAL, ModifierFlags::Alt));
    registerShortcut(ShortcutAction::DecreaseMeshResolution, "Decrease Mesh Resolution", "Mesh",
                     KeyCombination(GLFW_KEY_MINUS, ModifierFlags::Alt));
    registerShortcut(ShortcutAction::ResetMesh, "Reset Mesh", "Mesh",
                     KeyCombination(GLFW_KEY_R, ModifierFlags::Alt));

    // Navigation
    registerShortcut(ShortcutAction::NextQuad, "Next Quad", "Navigation",
                     KeyCombination(GLFW_KEY_TAB));
    registerShortcut(ShortcutAction::PreviousQuad, "Previous Quad", "Navigation",
                     KeyCombination(GLFW_KEY_TAB, ModifierFlags::Shift));

    // UI operations
    registerShortcut(ShortcutAction::ShowPreferences, "Preferences", "UI",
                     KeyCombination(GLFW_KEY_COMMA, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::ShowOutputConfig, "Output Configuration", "UI",
                     KeyCombination(GLFW_KEY_O, ModifierFlags::Alt));
    registerShortcut(ShortcutAction::ToggleInspector, "Toggle Inspector", "UI",
                     KeyCombination(GLFW_KEY_I, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::ToggleQuadList, "Toggle Quad List", "UI",
                     KeyCombination(GLFW_KEY_1, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::ToggleMediaBin, "Toggle Media Bin", "UI",
                     KeyCombination(GLFW_KEY_2, ModifierFlags::Ctrl));
    registerShortcut(ShortcutAction::ToggleVideoPlayer, "Toggle Video Player", "UI",
                     KeyCombination(GLFW_KEY_3, ModifierFlags::Ctrl));

    Logger::info("Keyboard shortcuts initialized with {} bindings", shortcuts_.size());
}

void KeyboardShortcuts::registerShortcut(ShortcutAction action, const std::string& name,
                                         const std::string& category, const KeyCombination& defaultKey) {
    ShortcutInfo info;
    info.action = action;
    info.name = name;
    info.category = category;
    info.defaultKey = defaultKey;
    info.currentKey = defaultKey;

    shortcuts_.push_back(info);

    if (defaultKey.isValid()) {
        keyToAction_[defaultKey] = action;
    }
}

void KeyboardShortcuts::registerCallback(ShortcutAction action, ShortcutCallback callback) {
    callbacks_[action].push_back(std::move(callback));
}

void KeyboardShortcuts::clearCallbacks(ShortcutAction action) {
    callbacks_.erase(action);
}

bool KeyboardShortcuts::processKeyEvent(int key, int /*scancode*/, int action, int mods) {
    // Only process key press events (not release or repeat)
    if (action != GLFW_PRESS) {
        return false;
    }

    ModifierFlags modFlags = fromGLFWMods(mods);
    KeyCombination combo(key, modFlags);

    auto it = keyToAction_.find(combo);
    if (it == keyToAction_.end()) {
        return false;
    }

    ShortcutAction shortcutAction = it->second;

    // Execute all callbacks for this action
    auto callbackIt = callbacks_.find(shortcutAction);
    if (callbackIt != callbacks_.end()) {
        for (const auto& callback : callbackIt->second) {
            callback();
        }
        Logger::debug("Shortcut triggered: {}", getActionName(shortcutAction));
        return true;
    }

    return false;
}

KeyCombination KeyboardShortcuts::getBinding(ShortcutAction action) const {
    size_t index = static_cast<size_t>(action);
    if (index < shortcuts_.size()) {
        return shortcuts_[index].currentKey;
    }
    return KeyCombination();
}

bool KeyboardShortcuts::setBinding(ShortcutAction action, const KeyCombination& key) {
    // Check for conflicts
    if (key.isValid()) {
        auto conflict = findConflict(key, action);
        if (conflict.has_value()) {
            Logger::warning("Key binding conflict: {} is already bound to {}",
                           key.toString(), getActionName(*conflict));
            return false;
        }
    }

    size_t index = static_cast<size_t>(action);
    if (index >= shortcuts_.size()) {
        return false;
    }

    // Remove old key from lookup
    KeyCombination oldKey = shortcuts_[index].currentKey;
    if (oldKey.isValid()) {
        keyToAction_.erase(oldKey);
    }

    // Set new key
    shortcuts_[index].currentKey = key;
    if (key.isValid()) {
        keyToAction_[key] = action;
    }

    Logger::info("Shortcut {} rebound to {}", getActionName(action), key.toString());
    return true;
}

void KeyboardShortcuts::resetBinding(ShortcutAction action) {
    size_t index = static_cast<size_t>(action);
    if (index >= shortcuts_.size()) {
        return;
    }

    // Remove current key from lookup
    KeyCombination oldKey = shortcuts_[index].currentKey;
    if (oldKey.isValid()) {
        keyToAction_.erase(oldKey);
    }

    // Reset to default
    shortcuts_[index].currentKey = shortcuts_[index].defaultKey;
    if (shortcuts_[index].defaultKey.isValid()) {
        keyToAction_[shortcuts_[index].defaultKey] = action;
    }
}

void KeyboardShortcuts::resetAllBindings() {
    keyToAction_.clear();
    for (auto& shortcut : shortcuts_) {
        shortcut.currentKey = shortcut.defaultKey;
        if (shortcut.defaultKey.isValid()) {
            keyToAction_[shortcut.defaultKey] = shortcut.action;
        }
    }
    Logger::info("All keyboard shortcuts reset to defaults");
}

std::optional<ShortcutAction> KeyboardShortcuts::findConflict(const KeyCombination& key,
                                                               ShortcutAction excludeAction) const {
    if (!key.isValid()) {
        return std::nullopt;
    }

    auto it = keyToAction_.find(key);
    if (it != keyToAction_.end() && it->second != excludeAction) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<ShortcutInfo> KeyboardShortcuts::getShortcutsByCategory(const std::string& category) const {
    std::vector<ShortcutInfo> result;
    for (const auto& shortcut : shortcuts_) {
        if (shortcut.category == category) {
            result.push_back(shortcut);
        }
    }
    return result;
}

std::vector<std::string> KeyboardShortcuts::getCategories() const {
    std::vector<std::string> categories;
    for (const auto& shortcut : shortcuts_) {
        if (std::find(categories.begin(), categories.end(), shortcut.category) == categories.end()) {
            categories.push_back(shortcut.category);
        }
    }
    return categories;
}

std::string KeyboardShortcuts::getActionName(ShortcutAction action) const {
    size_t index = static_cast<size_t>(action);
    if (index < shortcuts_.size()) {
        return shortcuts_[index].name;
    }
    return "Unknown";
}

std::string KeyboardShortcuts::getActionCategory(ShortcutAction action) const {
    size_t index = static_cast<size_t>(action);
    if (index < shortcuts_.size()) {
        return shortcuts_[index].category;
    }
    return "Unknown";
}

Result<void, Error> KeyboardShortcuts::saveToFile(const std::string& path) const {
    std::string json = toJson();
    return FileSystem::writeFile(path, json);
}

Result<void, Error> KeyboardShortcuts::loadFromFile(const std::string& path) {
    auto result = FileSystem::readFile(path);
    if (!result) {
        return Result<void, Error>::error(result.error());
    }
    return fromJson(result.value());
}

std::string KeyboardShortcuts::toJson() const {
    nlohmann::json root;
    root["version"] = "1.0";

    nlohmann::json bindings = nlohmann::json::object();
    for (const auto& shortcut : shortcuts_) {
        if (shortcut.isModified()) {
            bindings[shortcut.name] = shortcut.currentKey.toString();
        }
    }
    root["bindings"] = bindings;

    return root.dump(2);
}

Result<void, Error> KeyboardShortcuts::fromJson(const std::string& json) {
    try {
        nlohmann::json root = nlohmann::json::parse(json);

        if (!root.contains("version")) {
            return Result<void, Error>::error(Error("Missing version in shortcuts file"));
        }

        // Reset to defaults first
        resetAllBindings();

        // Apply custom bindings
        if (root.contains("bindings") && root["bindings"].is_object()) {
            for (const auto& [name, value] : root["bindings"].items()) {
                // Find action by name
                for (size_t i = 0; i < shortcuts_.size(); ++i) {
                    if (shortcuts_[i].name == name) {
                        KeyCombination key = KeyCombination::fromString(value.get<std::string>());
                        setBinding(shortcuts_[i].action, key);
                        break;
                    }
                }
            }
        }

        Logger::info("Loaded keyboard shortcuts from JSON");
        return Result<void, Error>::success();
    } catch (const nlohmann::json::exception& e) {
        return Result<void, Error>::error(Error("Failed to parse shortcuts JSON: " + std::string(e.what())));
    }
}

bool KeyboardShortcuts::hasModifiedBindings() const {
    for (const auto& shortcut : shortcuts_) {
        if (shortcut.isModified()) {
            return true;
        }
    }
    return false;
}

ModifierFlags KeyboardShortcuts::fromGLFWMods(int glfwMods) {
    ModifierFlags result = ModifierFlags::None;
    if (glfwMods & GLFW_MOD_CONTROL) {
        result = result | ModifierFlags::Ctrl;
    }
    if (glfwMods & GLFW_MOD_SHIFT) {
        result = result | ModifierFlags::Shift;
    }
    if (glfwMods & GLFW_MOD_ALT) {
        result = result | ModifierFlags::Alt;
    }
    if (glfwMods & GLFW_MOD_SUPER) {
        result = result | ModifierFlags::Super;
    }
    return result;
}

std::string KeyboardShortcuts::getKeyName(int glfwKey) {
    auto it = KEY_NAMES.find(glfwKey);
    if (it != KEY_NAMES.end()) {
        return it->second;
    }
    return "Unknown";
}

int KeyboardShortcuts::parseKeyName(const std::string& name) {
    auto it = KEY_CODES.find(name);
    if (it != KEY_CODES.end()) {
        return it->second;
    }
    // Try lowercase
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    it = KEY_CODES.find(lower);
    if (it != KEY_CODES.end()) {
        return it->second;
    }
    return GLFW_KEY_UNKNOWN;
}

} // namespace quadmapper
