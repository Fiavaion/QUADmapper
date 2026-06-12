#include "InputManager.hpp"
#include "core/Logger.hpp"
#include "core/FileSystem.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>

#ifdef _WIN32
#include <ShlObj.h>
#include <Windows.h>
#endif

namespace quadmapper {

// Get the path for keyboard shortcuts config file
static std::string getShortcutsConfigPath() {
    std::string configDir;

#ifdef _WIN32
    wchar_t* appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
        // Convert wide string to narrow string properly
        int size = WideCharToMultiByte(CP_UTF8, 0, appDataPath, -1, nullptr, 0, nullptr, nullptr);
        if (size > 0) {
            configDir.resize(static_cast<size_t>(size) - 1);
            WideCharToMultiByte(CP_UTF8, 0, appDataPath, -1, &configDir[0], size, nullptr, nullptr);
        }
        CoTaskMemFree(appDataPath);
    } else {
        configDir = ".";
    }
    configDir += "\\QUADmapper";
#else
    const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
    if (xdgConfig) {
        configDir = xdgConfig;
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            configDir = std::string(home) + "/.config";
        } else {
            configDir = ".";
        }
    }
    configDir += "/quadmapper";
#endif

    FileSystem::createDirectory(configDir);
    return configDir + "/shortcuts.json";
}

// Static instance pointer for GLFW callback
static InputManager* s_instance = nullptr;

InputManager::InputManager() {
    // Shortcuts are initialized in their constructor
}

InputManager::~InputManager() {
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

bool InputManager::initialize(GLFWwindow* window) {
    if (!window) {
        Logger::error("InputManager: Cannot initialize with null window");
        return false;
    }

    window_ = window;
    s_instance = this;

    // Set up GLFW key callback
    glfwSetKeyCallback(window, keyCallback);

    // Load saved shortcut preferences
    loadShortcutPreferences();

    Logger::info("InputManager initialized");
    return true;
}

void InputManager::update() {
    // Update modifier state from GLFW
    if (window_) {
        ctrlHeld_ = glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                    glfwGetKey(window_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        shiftHeld_ = glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                     glfwGetKey(window_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        altHeld_ = glfwGetKey(window_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                   glfwGetKey(window_, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
    }
}

bool InputManager::processKeyEvent(int key, int scancode, int action, int mods) {
    // Don't process shortcuts if disabled or if text input is active
    if (!shortcutsEnabled_ || isTextInputActive()) {
        return false;
    }

    return shortcuts_.processKeyEvent(key, scancode, action, mods);
}

bool InputManager::isTextInputActive() const {
    ImGuiIO& io = ImGui::GetIO();
    return io.WantTextInput;
}

void InputManager::loadShortcutPreferences() {
    std::string path = getShortcutsConfigPath();

    auto result = shortcuts_.loadFromFile(path);
    if (!result) {
        Logger::debug("No saved keyboard shortcuts found, using defaults");
    }
}

void InputManager::saveShortcutPreferences() {
    std::string path = getShortcutsConfigPath();

    auto result = shortcuts_.saveToFile(path);
    if (!result) {
        Logger::warning("Failed to save keyboard shortcuts: {}", result.error().message);
    }
}

void InputManager::keyCallback(GLFWwindow* /*window*/, int key, int scancode, int action, int mods) {
    // Let ImGui process the key first
    // ImGui_ImplGlfw already handles this via its own callback chain

    // Process our shortcuts if we have an instance
    if (s_instance) {
        s_instance->processKeyEvent(key, scancode, action, mods);
    }
}

} // namespace quadmapper
