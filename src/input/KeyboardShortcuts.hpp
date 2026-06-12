#pragma once

#include "core/Result.hpp"
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include <optional>

// Forward declare GLFW constants
#ifndef GLFW_KEY_UNKNOWN
#define GLFW_KEY_UNKNOWN -1
#endif

namespace quadmapper {

/**
 * All shortcut-able actions in QUADmapper.
 * These follow Resolume-style conventions where practical.
 */
enum class ShortcutAction {
    // File operations
    NewProject,
    OpenProject,
    SaveProject,
    SaveProjectAs,

    // Edit operations
    Undo,
    Redo,
    Cut,
    Copy,
    Paste,
    Delete,
    SelectAll,
    DeselectAll,
    Duplicate,

    // View operations
    ToggleFullscreen,
    ZoomIn,
    ZoomOut,
    ZoomReset,
    FitToWindow,

    // Quad operations
    AddQuad,
    GroupQuads,
    UngroupQuads,
    LockQuad,
    UnlockQuad,
    HideQuad,
    ShowQuad,

    // Z-order operations
    BringToFront,
    BringForward,
    SendBackward,
    SendToBack,

    // Playback operations (Resolume-style)
    PlayPause,
    Stop,
    NextFrame,
    PreviousFrame,
    SkipForward,
    SkipBackward,
    LoopToggle,

    // Output operations
    ToggleOutput,
    TogglePerformMode,
    BlackoutOutput,

    // Mesh operations
    ToggleMeshEdit,
    IncreaseMeshResolution,
    DecreaseMeshResolution,
    ResetMesh,

    // Navigation
    NextQuad,
    PreviousQuad,

    // UI operations
    ShowPreferences,
    ShowOutputConfig,
    ToggleInspector,
    ToggleQuadList,
    ToggleMediaBin,
    ToggleVideoPlayer,

    // Count marker (must be last)
    COUNT
};

/**
 * Modifier key flags that can be combined.
 */
enum class ModifierFlags : uint8_t {
    None = 0,
    Ctrl = 1 << 0,
    Shift = 1 << 1,
    Alt = 1 << 2,
    Super = 1 << 3  // Windows/Command key
};

// Enable bitwise operations for ModifierFlags
inline ModifierFlags operator|(ModifierFlags a, ModifierFlags b) {
    return static_cast<ModifierFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline ModifierFlags operator&(ModifierFlags a, ModifierFlags b) {
    return static_cast<ModifierFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool hasFlag(ModifierFlags flags, ModifierFlags flag) {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

/**
 * A key combination consisting of a key code and modifier flags.
 */
struct KeyCombination {
    int key = GLFW_KEY_UNKNOWN;  // GLFW key code
    ModifierFlags modifiers = ModifierFlags::None;

    KeyCombination() = default;
    KeyCombination(int k) : key(k), modifiers(ModifierFlags::None) {}
    KeyCombination(int k, ModifierFlags mods) : key(k), modifiers(mods) {}

    bool isValid() const { return key != GLFW_KEY_UNKNOWN; }

    bool operator==(const KeyCombination& other) const {
        return key == other.key && modifiers == other.modifiers;
    }

    bool operator!=(const KeyCombination& other) const {
        return !(*this == other);
    }

    /**
     * Check if this combination matches the given key event.
     */
    bool matches(int eventKey, ModifierFlags eventMods) const {
        return key == eventKey && modifiers == eventMods;
    }

    /**
     * Get a human-readable string for this key combination.
     */
    std::string toString() const;

    /**
     * Parse a key combination from a string (e.g., "Ctrl+S").
     */
    static KeyCombination fromString(const std::string& str);
};

/**
 * Hash function for KeyCombination to use in unordered_map.
 */
struct KeyCombinationHash {
    size_t operator()(const KeyCombination& kc) const {
        return std::hash<int>()(kc.key) ^
               (std::hash<uint8_t>()(static_cast<uint8_t>(kc.modifiers)) << 16);
    }
};

/**
 * Information about a shortcut action.
 */
struct ShortcutInfo {
    ShortcutAction action;
    std::string name;           // Display name (e.g., "Save Project")
    std::string category;       // Category for UI grouping (e.g., "File")
    KeyCombination defaultKey;  // Default key binding
    KeyCombination currentKey;  // Current key binding (may differ from default)

    bool isModified() const { return currentKey != defaultKey; }
};

/**
 * Callback type for shortcut actions.
 */
using ShortcutCallback = std::function<void()>;

/**
 * Manages keyboard shortcuts for the application.
 * Supports configurable bindings with save/load to JSON.
 */
class KeyboardShortcuts {
public:
    KeyboardShortcuts();
    ~KeyboardShortcuts() = default;

    // Non-copyable
    KeyboardShortcuts(const KeyboardShortcuts&) = delete;
    KeyboardShortcuts& operator=(const KeyboardShortcuts&) = delete;

    /**
     * Initialize default Resolume-style key bindings.
     */
    void initializeDefaults();

    /**
     * Register a callback for a specific action.
     * Multiple callbacks can be registered for the same action.
     */
    void registerCallback(ShortcutAction action, ShortcutCallback callback);

    /**
     * Clear all callbacks for an action.
     */
    void clearCallbacks(ShortcutAction action);

    /**
     * Process a key event. Returns true if a shortcut was triggered.
     */
    bool processKeyEvent(int key, int scancode, int action, int mods);

    /**
     * Get the current key binding for an action.
     */
    KeyCombination getBinding(ShortcutAction action) const;

    /**
     * Set a new key binding for an action.
     * Returns false if the binding conflicts with another action.
     */
    bool setBinding(ShortcutAction action, const KeyCombination& key);

    /**
     * Reset a binding to its default.
     */
    void resetBinding(ShortcutAction action);

    /**
     * Reset all bindings to defaults.
     */
    void resetAllBindings();

    /**
     * Check if a key combination is already bound to another action.
     * Returns the conflicting action if found.
     */
    std::optional<ShortcutAction> findConflict(const KeyCombination& key,
                                                ShortcutAction excludeAction) const;

    /**
     * Get information about all shortcuts.
     */
    const std::vector<ShortcutInfo>& getAllShortcuts() const { return shortcuts_; }

    /**
     * Get shortcuts by category.
     */
    std::vector<ShortcutInfo> getShortcutsByCategory(const std::string& category) const;

    /**
     * Get all unique categories.
     */
    std::vector<std::string> getCategories() const;

    /**
     * Get the display name for an action.
     */
    std::string getActionName(ShortcutAction action) const;

    /**
     * Get the category for an action.
     */
    std::string getActionCategory(ShortcutAction action) const;

    /**
     * Save shortcuts to a JSON file.
     */
    Result<void, Error> saveToFile(const std::string& path) const;

    /**
     * Load shortcuts from a JSON file.
     */
    Result<void, Error> loadFromFile(const std::string& path);

    /**
     * Serialize shortcuts to JSON string.
     */
    std::string toJson() const;

    /**
     * Deserialize shortcuts from JSON string.
     */
    Result<void, Error> fromJson(const std::string& json);

    /**
     * Check if any bindings have been modified from defaults.
     */
    bool hasModifiedBindings() const;

    /**
     * Convert GLFW modifier flags to our ModifierFlags.
     */
    static ModifierFlags fromGLFWMods(int glfwMods);

    /**
     * Get the key name for a GLFW key code.
     */
    static std::string getKeyName(int glfwKey);

    /**
     * Parse a key name to GLFW key code.
     */
    static int parseKeyName(const std::string& name);

private:
    void registerShortcut(ShortcutAction action, const std::string& name,
                          const std::string& category, const KeyCombination& defaultKey);

    std::vector<ShortcutInfo> shortcuts_;
    std::unordered_map<KeyCombination, ShortcutAction, KeyCombinationHash> keyToAction_;
    std::unordered_map<ShortcutAction, std::vector<ShortcutCallback>> callbacks_;
};

} // namespace quadmapper
