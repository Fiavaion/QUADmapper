#pragma once

#include "KeyboardShortcuts.hpp"
#include <memory>

struct GLFWwindow;

namespace quadmapper {

/**
 * Manages all input handling for the application.
 * Coordinates keyboard shortcuts and mouse handling.
 */
class InputManager {
public:
    InputManager();
    ~InputManager();

    // Non-copyable
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    /**
     * Initialize the input manager and set up GLFW callbacks.
     */
    bool initialize(GLFWwindow* window);

    /**
     * Process input for the current frame.
     * Call once per frame after glfwPollEvents().
     */
    void update();

    /**
     * Get the keyboard shortcuts manager.
     */
    KeyboardShortcuts& getKeyboardShortcuts() { return shortcuts_; }
    const KeyboardShortcuts& getKeyboardShortcuts() const { return shortcuts_; }

    /**
     * Process a key event from GLFW callback.
     * Returns true if the event was consumed by a shortcut.
     */
    bool processKeyEvent(int key, int scancode, int action, int mods);

    /**
     * Check if a modifier key is currently held.
     */
    bool isCtrlHeld() const { return ctrlHeld_; }
    bool isShiftHeld() const { return shiftHeld_; }
    bool isAltHeld() const { return altHeld_; }

    /**
     * Check if text input is currently active (e.g., in an ImGui text field).
     * Shortcuts should be disabled when this returns true.
     */
    bool isTextInputActive() const;

    /**
     * Set whether shortcuts should be globally disabled.
     * Useful during modal dialogs or capture modes.
     */
    void setShortcutsEnabled(bool enabled) { shortcutsEnabled_ = enabled; }
    bool areShortcutsEnabled() const { return shortcutsEnabled_; }

    /**
     * Load keyboard shortcut preferences from file.
     */
    void loadShortcutPreferences();

    /**
     * Save keyboard shortcut preferences to file.
     */
    void saveShortcutPreferences();

private:
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

    GLFWwindow* window_ = nullptr;
    KeyboardShortcuts shortcuts_;

    bool shortcutsEnabled_ = true;
    bool ctrlHeld_ = false;
    bool shiftHeld_ = false;
    bool altHeld_ = false;
};

} // namespace quadmapper
