#include "PreferencesDialog.hpp"
#include "core/Logger.hpp"
#include "core/FileSystem.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <ShlObj.h>
#include <Windows.h>
#endif

namespace quadmapper {

#ifdef _WIN32
// Open folder browser dialog and return selected path
static std::string openFolderDialog(const char* title = "Select Folder") {
    char path[MAX_PATH] = "";
    BROWSEINFOA bi = {};
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != nullptr) {
        SHGetPathFromIDListA(pidl, path);
        CoTaskMemFree(pidl);
    }
    return std::string(path);
}
#endif

std::string AppPreferences::getDefaultConfigPath() {
    std::string configDir;

#ifdef _WIN32
    // Get APPDATA path on Windows
    wchar_t* appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
        std::wstring wpath(appDataPath);
        configDir = std::string(wpath.begin(), wpath.end());
        CoTaskMemFree(appDataPath);
    } else {
        configDir = ".";
    }
    configDir += "\\QUADmapper";
#else
    // Use XDG config home on Linux/Mac
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

    // Ensure directory exists
    FileSystem::createDirectory(configDir);

    return configDir + "/preferences.json";
}

PreferencesDialog::PreferencesDialog() {
    resetToDefaults();
    savedPreferences_ = preferences_;
}

void PreferencesDialog::show() {
    visible_ = true;
    savedPreferences_ = preferences_;
    Logger::debug("Preferences dialog opened");
}

void PreferencesDialog::hide() {
    visible_ = false;
    capturingShortcut_ = false;
}

void PreferencesDialog::setKeyboardShortcuts(KeyboardShortcuts* shortcuts) {
    shortcuts_ = shortcuts;
}

void PreferencesDialog::render() {
    if (!visible_) {
        return;
    }

    // Handle shortcut capture
    if (capturingShortcut_) {
        handleShortcutCapture();
    }

    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                           ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Preferences", &visible_, ImGuiWindowFlags_NoCollapse)) {
        // Tab bar
        if (ImGui::BeginTabBar("PreferenceTabs")) {
            if (ImGui::BeginTabItem("General")) {
                currentTab_ = 0;
                renderGeneralTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Keyboard Shortcuts")) {
                currentTab_ = 1;
                renderKeyboardShortcutsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Performance")) {
                currentTab_ = 2;
                renderPerformanceTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();

        // Bottom buttons
        float buttonWidth = 100;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float totalWidth = buttonWidth * 3 + spacing * 2;
        float availWidth = ImGui::GetContentRegionAvail().x;

        ImGui::SetCursorPosX((availWidth - totalWidth) * 0.5f + ImGui::GetCursorPosX());

        if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) {
            savePreferences();
            hide();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
            // Restore saved preferences
            preferences_ = savedPreferences_;
            modified_ = false;
            hide();
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply", ImVec2(buttonWidth, 0))) {
            savePreferences();
        }
    }
    ImGui::End();
}

void PreferencesDialog::renderGeneralTab() {
    ImGui::BeginChild("GeneralSettings", ImVec2(0, -40), true);

    // Project Defaults section
    if (ImGui::CollapsingHeader("Project Defaults", ImGuiTreeNodeFlags_DefaultOpen)) {
        static char projectPathBuf[512];
        std::strncpy(projectPathBuf, preferences_.general.defaultProjectPath.c_str(),
                    sizeof(projectPathBuf) - 1);
        projectPathBuf[sizeof(projectPathBuf) - 1] = '\0';

        ImGui::Text("Default Project Location:");
        if (ImGui::InputText("##projectPath", projectPathBuf, sizeof(projectPathBuf))) {
            preferences_.general.defaultProjectPath = projectPathBuf;
            modified_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...##projectPath")) {
            // TODO: Open folder browser dialog
        }

        if (ImGui::SliderInt("Default Mesh Resolution", &preferences_.general.defaultMeshResolution, 2, 64)) {
            modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Default mesh resolution for new quads (2-64)");
        }

        if (ImGui::SliderInt("Recent Projects Max", &preferences_.general.recentProjectsMaxCount, 5, 25)) {
            modified_ = true;
        }

        if (ImGui::Checkbox("Open Last Project on Startup", &preferences_.general.openLastProjectOnStartup)) {
            modified_ = true;
        }
    }

    // Media Library section
    if (ImGui::CollapsingHeader("Media Library", ImGuiTreeNodeFlags_DefaultOpen)) {
        static char mediaFolderBuf[512];
        std::strncpy(mediaFolderBuf, preferences_.general.defaultMediaFolder.c_str(),
                    sizeof(mediaFolderBuf) - 1);
        mediaFolderBuf[sizeof(mediaFolderBuf) - 1] = '\0';

        ImGui::Text("Default Media Folder:");
        if (ImGui::InputText("##mediaFolder", mediaFolderBuf, sizeof(mediaFolderBuf))) {
            preferences_.general.defaultMediaFolder = mediaFolderBuf;
            modified_ = true;
        }
        ImGui::SameLine();
#ifdef _WIN32
        if (ImGui::Button("Browse...##mediaFolder")) {
            std::string folder = openFolderDialog("Select Default Media Folder");
            if (!folder.empty()) {
                preferences_.general.defaultMediaFolder = folder;
                std::strncpy(mediaFolderBuf, folder.c_str(), sizeof(mediaFolderBuf) - 1);
                mediaFolderBuf[sizeof(mediaFolderBuf) - 1] = '\0';
                modified_ = true;
            }
        }
#else
        ImGui::BeginDisabled(true);
        ImGui::Button("Browse...##mediaFolder");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Folder browser not available on this platform");
        }
#endif
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select a folder to auto-load media from");
        }

        if (ImGui::Checkbox("Auto-load Media on Startup", &preferences_.general.autoLoadMediaOnStartup)) {
            modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically import media files from the default folder on startup");
        }

        ImGui::BeginDisabled(!preferences_.general.autoLoadMediaOnStartup);
        if (ImGui::Checkbox("Scan Subdirectories", &preferences_.general.recursiveFolderScan)) {
            modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Include files from subdirectories when scanning");
        }
        ImGui::EndDisabled();

        // Folder bookmarks
        ImGui::Separator();
        ImGui::Text("Folder Bookmarks:");

        auto& bookmarks = preferences_.general.mediaFolderBookmarks;
        int toRemove = -1;
        for (size_t i = 0; i < bookmarks.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));

            // Display folder name (last component of path)
            std::filesystem::path p(bookmarks[i]);
            ImGui::BulletText("%s", p.filename().string().c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", bookmarks[i].c_str());
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("X##remove")) {
                toRemove = static_cast<int>(i);
                modified_ = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove bookmark");
            }

            ImGui::PopID();
        }

        if (toRemove >= 0 && toRemove < static_cast<int>(bookmarks.size())) {
            bookmarks.erase(bookmarks.begin() + toRemove);
        }

#ifdef _WIN32
        if (ImGui::Button("+ Add Bookmark")) {
            std::string folder = openFolderDialog("Add Folder Bookmark");
            if (!folder.empty()) {
                // Check for duplicates
                bool isDuplicate = false;
                for (const auto& bm : bookmarks) {
                    if (bm == folder) {
                        isDuplicate = true;
                        break;
                    }
                }
                if (!isDuplicate) {
                    bookmarks.push_back(folder);
                    modified_ = true;
                }
            }
        }
#else
        ImGui::BeginDisabled(true);
        ImGui::Button("+ Add Bookmark");
        ImGui::EndDisabled();
#endif
    }

    // Auto-save section
    if (ImGui::CollapsingHeader("Auto-Save", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enable Auto-Save", &preferences_.general.autoSaveEnabled)) {
            modified_ = true;
        }

        ImGui::BeginDisabled(!preferences_.general.autoSaveEnabled);
        if (ImGui::SliderInt("Auto-Save Interval (minutes)", &preferences_.general.autoSaveIntervalMinutes, 1, 30)) {
            modified_ = true;
        }
        ImGui::EndDisabled();

        if (ImGui::Checkbox("Create Backup on Save", &preferences_.general.createBackupOnSave)) {
            modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Creates a .bak file before overwriting");
        }
    }

    // User Interface section
    if (ImGui::CollapsingHeader("User Interface", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Show Welcome Dialog on Startup", &preferences_.general.showWelcomeDialog)) {
            modified_ = true;
        }

        if (ImGui::Checkbox("Confirm Before Exit", &preferences_.general.confirmOnExit)) {
            modified_ = true;
        }

        if (ImGui::Checkbox("Remember Window Layout", &preferences_.general.rememberWindowLayout)) {
            modified_ = true;
        }

        const char* themes[] = {"Dark", "Light"};
        if (ImGui::Combo("Theme", &preferences_.general.themeIndex, themes, 2)) {
            modified_ = true;
            // Apply theme immediately
            if (preferences_.general.themeIndex == 0) {
                ImGui::StyleColorsDark();
            } else {
                ImGui::StyleColorsLight();
            }
        }
    }

    ImGui::EndChild();
}

void PreferencesDialog::renderKeyboardShortcutsTab() {
    if (!shortcuts_) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Keyboard shortcuts not available");
        return;
    }

    // Top controls
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Filter:");
    ImGui::SameLine();

    // Category filter
    auto categories = shortcuts_->getCategories();
    categories.insert(categories.begin(), "All");

    const char* currentCategory = shortcutFilterCategory_.empty() ?
                                  "All" : shortcutFilterCategory_.c_str();

    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("##categoryFilter", currentCategory)) {
        for (const auto& cat : categories) {
            bool isSelected = (cat == "All" && shortcutFilterCategory_.empty()) ||
                             (cat == shortcutFilterCategory_);
            if (ImGui::Selectable(cat.c_str(), isSelected)) {
                shortcutFilterCategory_ = (cat == "All") ? "" : cat;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Text("Search:");
    ImGui::SameLine();

    static char searchBuf[128] = "";
    ImGui::SetNextItemWidth(150);
    if (ImGui::InputText("##searchFilter", searchBuf, sizeof(searchBuf))) {
        shortcutSearchFilter_ = searchBuf;
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset All")) {
        shortcuts_->resetAllBindings();
        modified_ = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reset all shortcuts to defaults");
    }

    ImGui::Separator();

    // Shortcuts list
    ImGui::BeginChild("ShortcutsList", ImVec2(0, -60), true);

    // Prepare lowercase search filter
    std::string searchLower = shortcutSearchFilter_;
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);

    // Display shortcuts grouped by category
    std::string lastCategory;
    for (auto& shortcut : const_cast<std::vector<ShortcutInfo>&>(shortcuts_->getAllShortcuts())) {
        // Apply filters
        if (!shortcutFilterCategory_.empty() && shortcut.category != shortcutFilterCategory_) {
            continue;
        }
        if (!searchLower.empty()) {
            std::string nameLower = shortcut.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            if (nameLower.find(searchLower) == std::string::npos) {
                continue;
            }
        }

        // Category header
        if (shortcut.category != lastCategory) {
            if (!lastCategory.empty()) {
                ImGui::Separator();
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "%s", shortcut.category.c_str());
            lastCategory = shortcut.category;
        }

        renderShortcutEntry(shortcut);
    }

    ImGui::EndChild();

    // Capture mode indicator
    if (capturingShortcut_) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1),
                          "Press a key combination for '%s'... (%.1fs)",
                          shortcuts_->getActionName(captureAction_).c_str(),
                          captureTimeRemaining_);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            stopShortcutCapture();
        }
    } else {
        ImGui::TextDisabled("Click a shortcut button to change it. Press Escape to clear.");
    }
}

void PreferencesDialog::renderShortcutEntry(ShortcutInfo& info) {
    ImGui::PushID(static_cast<int>(info.action));

    // Action name
    float nameWidth = 200;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", info.name.c_str());

    // Shortcut button (right-aligned)
    float buttonWidth = 150;
    float resetButtonWidth = 50;
    float availWidth = ImGui::GetContentRegionAvail().x;

    ImGui::SameLine(nameWidth);

    // Check if currently capturing this shortcut
    bool isCapturing = (capturingShortcut_ && captureAction_ == info.action);

    if (isCapturing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.6f, 0.0f, 1.0f));
        ImGui::Button("Press key...", ImVec2(buttonWidth, 0));
        ImGui::PopStyleColor();
    } else {
        std::string keyStr = info.currentKey.toString();
        if (info.isModified()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
        }
        if (ImGui::Button(keyStr.c_str(), ImVec2(buttonWidth, 0))) {
            startShortcutCapture(info.action);
        }
        if (info.isModified()) {
            ImGui::PopStyleColor();
        }
    }

    // Reset button (only if modified)
    ImGui::SameLine();
    ImGui::BeginDisabled(!info.isModified());
    if (ImGui::Button("Reset", ImVec2(resetButtonWidth, 0))) {
        shortcuts_->resetBinding(info.action);
        modified_ = true;
    }
    ImGui::EndDisabled();

    ImGui::PopID();
}

void PreferencesDialog::startShortcutCapture(ShortcutAction action) {
    capturingShortcut_ = true;
    captureAction_ = action;
    captureTimeRemaining_ = 5.0f;  // 5 second timeout
    Logger::debug("Started shortcut capture for: {}", shortcuts_->getActionName(action));
}

void PreferencesDialog::stopShortcutCapture() {
    capturingShortcut_ = false;
    captureAction_ = ShortcutAction::COUNT;
    captureTimeRemaining_ = 0.0f;
}

void PreferencesDialog::handleShortcutCapture() {
    if (!capturingShortcut_) {
        return;
    }

    // Update timeout
    captureTimeRemaining_ -= ImGui::GetIO().DeltaTime;
    if (captureTimeRemaining_ <= 0) {
        stopShortcutCapture();
        return;
    }

    // Check for key presses
    ImGuiIO& io = ImGui::GetIO();

    // Build modifier flags
    ModifierFlags mods = ModifierFlags::None;
    if (io.KeyCtrl) mods = mods | ModifierFlags::Ctrl;
    if (io.KeyShift) mods = mods | ModifierFlags::Shift;
    if (io.KeyAlt) mods = mods | ModifierFlags::Alt;
    if (io.KeySuper) mods = mods | ModifierFlags::Super;

    // Check for Escape to clear the binding
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (mods == ModifierFlags::None) {
            // Clear the binding
            shortcuts_->setBinding(captureAction_, KeyCombination());
            modified_ = true;
            stopShortcutCapture();
            return;
        }
    }

    // Check for regular key presses (not just modifiers)
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        // Skip modifier keys themselves
        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
            key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
            key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
            key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) {
            continue;
        }

        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key))) {
            // Convert ImGuiKey to GLFW key
            int glfwKey = GLFW_KEY_UNKNOWN;

            // Map common keys
            if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
                glfwKey = GLFW_KEY_A + (key - ImGuiKey_A);
            } else if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
                glfwKey = GLFW_KEY_0 + (key - ImGuiKey_0);
            } else if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) {
                glfwKey = GLFW_KEY_F1 + (key - ImGuiKey_F1);
            } else {
                // Map special keys
                switch (key) {
                    case ImGuiKey_Space: glfwKey = GLFW_KEY_SPACE; break;
                    case ImGuiKey_Enter: glfwKey = GLFW_KEY_ENTER; break;
                    case ImGuiKey_Tab: glfwKey = GLFW_KEY_TAB; break;
                    case ImGuiKey_Backspace: glfwKey = GLFW_KEY_BACKSPACE; break;
                    case ImGuiKey_Delete: glfwKey = GLFW_KEY_DELETE; break;
                    case ImGuiKey_Insert: glfwKey = GLFW_KEY_INSERT; break;
                    case ImGuiKey_Home: glfwKey = GLFW_KEY_HOME; break;
                    case ImGuiKey_End: glfwKey = GLFW_KEY_END; break;
                    case ImGuiKey_PageUp: glfwKey = GLFW_KEY_PAGE_UP; break;
                    case ImGuiKey_PageDown: glfwKey = GLFW_KEY_PAGE_DOWN; break;
                    case ImGuiKey_LeftArrow: glfwKey = GLFW_KEY_LEFT; break;
                    case ImGuiKey_RightArrow: glfwKey = GLFW_KEY_RIGHT; break;
                    case ImGuiKey_UpArrow: glfwKey = GLFW_KEY_UP; break;
                    case ImGuiKey_DownArrow: glfwKey = GLFW_KEY_DOWN; break;
                    case ImGuiKey_LeftBracket: glfwKey = GLFW_KEY_LEFT_BRACKET; break;
                    case ImGuiKey_RightBracket: glfwKey = GLFW_KEY_RIGHT_BRACKET; break;
                    case ImGuiKey_Backslash: glfwKey = GLFW_KEY_BACKSLASH; break;
                    case ImGuiKey_Semicolon: glfwKey = GLFW_KEY_SEMICOLON; break;
                    case ImGuiKey_Apostrophe: glfwKey = GLFW_KEY_APOSTROPHE; break;
                    case ImGuiKey_Comma: glfwKey = GLFW_KEY_COMMA; break;
                    case ImGuiKey_Period: glfwKey = GLFW_KEY_PERIOD; break;
                    case ImGuiKey_Slash: glfwKey = GLFW_KEY_SLASH; break;
                    case ImGuiKey_Minus: glfwKey = GLFW_KEY_MINUS; break;
                    case ImGuiKey_Equal: glfwKey = GLFW_KEY_EQUAL; break;
                    case ImGuiKey_GraveAccent: glfwKey = GLFW_KEY_GRAVE_ACCENT; break;
                    default: break;
                }
            }

            if (glfwKey != GLFW_KEY_UNKNOWN) {
                KeyCombination newKey(glfwKey, mods);

                // Check for conflicts
                auto conflict = shortcuts_->findConflict(newKey, captureAction_);
                if (conflict.has_value()) {
                    Logger::warning("Key conflict with: {}", shortcuts_->getActionName(*conflict));
                    // Could show a popup here asking to override
                }

                if (shortcuts_->setBinding(captureAction_, newKey)) {
                    modified_ = true;
                }
                stopShortcutCapture();
                return;
            }
        }
    }
}

void PreferencesDialog::renderPerformanceTab() {
    ImGui::BeginChild("PerformanceSettings", ImVec2(0, -40), true);

    // Video Decoding section
    if (ImGui::CollapsingHeader("Video Decoding", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SliderInt("Video Buffer Frames", &preferences_.performance.videoBufferFrames, 10, 120)) {
            modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Number of frames to buffer ahead (higher = smoother, more memory)");
        }

        if (ImGui::SliderInt("Decode Threads", &preferences_.performance.videoDecodeThreads, 1, 8)) {
            modified_ = true;
        }

        if (ImGui::Checkbox("Enable Hardware Decoding", &preferences_.performance.enableHardwareDecoding)) {
            modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Use GPU-accelerated video decoding when available");
        }
    }

    // Rendering section
    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enable VSync", &preferences_.performance.enableVSync)) {
            modified_ = true;
        }

        ImGui::BeginDisabled(preferences_.performance.enableVSync);
        if (ImGui::SliderInt("Target Frame Rate", &preferences_.performance.targetFrameRate, 30, 240)) {
            modified_ = true;
        }
        ImGui::EndDisabled();

        const char* qualityLevels[] = {"Low", "Medium", "High"};
        if (ImGui::Combo("Texture Quality", &preferences_.performance.textureQuality, qualityLevels, 3)) {
            modified_ = true;
        }

        if (ImGui::Checkbox("Enable Mipmaps", &preferences_.performance.enableMipmaps)) {
            modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Generate mipmaps for smoother texture scaling");
        }
    }

    // Audio section
    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* bufferSizes[] = {"128", "256", "512", "1024", "2048"};
        int bufferSizeIndex = 2;  // Default 512
        switch (preferences_.performance.audioBufferSize) {
            case 128: bufferSizeIndex = 0; break;
            case 256: bufferSizeIndex = 1; break;
            case 512: bufferSizeIndex = 2; break;
            case 1024: bufferSizeIndex = 3; break;
            case 2048: bufferSizeIndex = 4; break;
        }
        if (ImGui::Combo("Buffer Size (samples)", &bufferSizeIndex, bufferSizes, 5)) {
            const int sizes[] = {128, 256, 512, 1024, 2048};
            preferences_.performance.audioBufferSize = sizes[bufferSizeIndex];
            modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Lower = less latency, higher CPU usage");
        }

        const char* sampleRates[] = {"44100 Hz", "48000 Hz", "96000 Hz"};
        int sampleRateIndex = 1;  // Default 48000
        switch (preferences_.performance.audioSampleRate) {
            case 44100: sampleRateIndex = 0; break;
            case 48000: sampleRateIndex = 1; break;
            case 96000: sampleRateIndex = 2; break;
        }
        if (ImGui::Combo("Sample Rate", &sampleRateIndex, sampleRates, 3)) {
            const int rates[] = {44100, 48000, 96000};
            preferences_.performance.audioSampleRate = rates[sampleRateIndex];
            modified_ = true;
        }
    }

    // Output section
    if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* bufferCounts[] = {"Double Buffering", "Triple Buffering"};
        int bufferCountIndex = preferences_.performance.outputBufferCount - 2;
        if (bufferCountIndex < 0) bufferCountIndex = 0;
        if (ImGui::Combo("Buffer Mode", &bufferCountIndex, bufferCounts, 2)) {
            preferences_.performance.outputBufferCount = bufferCountIndex + 2;
            modified_ = true;
        }

        if (ImGui::Checkbox("Enable Adaptive Sync", &preferences_.performance.enableAdaptiveSync)) {
            modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Use G-Sync/FreeSync when available");
        }
    }

    ImGui::EndChild();
}

Result<void, Error> PreferencesDialog::loadPreferences() {
    return loadFromFile(AppPreferences::getDefaultConfigPath());
}

Result<void, Error> PreferencesDialog::savePreferences() {
    auto result = saveToFile(AppPreferences::getDefaultConfigPath());
    if (result) {
        savedPreferences_ = preferences_;
        modified_ = false;

        // Also save keyboard shortcuts
        if (shortcuts_) {
            std::string shortcutsPath = AppPreferences::getDefaultConfigPath();
            size_t lastDot = shortcutsPath.rfind('.');
            if (lastDot != std::string::npos) {
                shortcutsPath = shortcutsPath.substr(0, lastDot) + "_shortcuts.json";
            } else {
                shortcutsPath += "_shortcuts.json";
            }
            shortcuts_->saveToFile(shortcutsPath);
        }
    }
    return result;
}

Result<void, Error> PreferencesDialog::loadFromFile(const std::string& path) {
    auto result = FileSystem::readFile(path);
    if (!result) {
        Logger::warning("Could not load preferences from {}: {}", path, result.error().message);
        return Result<void, Error>::error(result.error());
    }

    try {
        nlohmann::json root = nlohmann::json::parse(result.value());

        if (!root.contains("version")) {
            return Result<void, Error>::error(Error("Missing version in preferences file"));
        }

        // Load general preferences
        if (root.contains("general")) {
            const auto& gen = root["general"];
            if (gen.contains("defaultProjectPath")) {
                preferences_.general.defaultProjectPath = gen["defaultProjectPath"];
            }
            if (gen.contains("defaultMeshResolution")) {
                preferences_.general.defaultMeshResolution = gen["defaultMeshResolution"];
            }
            if (gen.contains("openLastProjectOnStartup")) {
                preferences_.general.openLastProjectOnStartup = gen["openLastProjectOnStartup"];
            }
            if (gen.contains("recentProjectsMaxCount")) {
                preferences_.general.recentProjectsMaxCount = gen["recentProjectsMaxCount"];
            }
            if (gen.contains("autoSaveEnabled")) {
                preferences_.general.autoSaveEnabled = gen["autoSaveEnabled"];
            }
            if (gen.contains("autoSaveIntervalMinutes")) {
                preferences_.general.autoSaveIntervalMinutes = gen["autoSaveIntervalMinutes"];
            }
            if (gen.contains("createBackupOnSave")) {
                preferences_.general.createBackupOnSave = gen["createBackupOnSave"];
            }
            if (gen.contains("showWelcomeDialog")) {
                preferences_.general.showWelcomeDialog = gen["showWelcomeDialog"];
            }
            if (gen.contains("confirmOnExit")) {
                preferences_.general.confirmOnExit = gen["confirmOnExit"];
            }
            if (gen.contains("rememberWindowLayout")) {
                preferences_.general.rememberWindowLayout = gen["rememberWindowLayout"];
            }
            if (gen.contains("themeIndex")) {
                preferences_.general.themeIndex = gen["themeIndex"];
            }
            if (gen.contains("uiMode")) {
                preferences_.general.uiMode = gen["uiMode"];
            }
            // Media Library settings
            if (gen.contains("defaultMediaFolder")) {
                preferences_.general.defaultMediaFolder = gen["defaultMediaFolder"];
            }
            if (gen.contains("autoLoadMediaOnStartup")) {
                preferences_.general.autoLoadMediaOnStartup = gen["autoLoadMediaOnStartup"];
            }
            if (gen.contains("recursiveFolderScan")) {
                preferences_.general.recursiveFolderScan = gen["recursiveFolderScan"];
            }
            if (gen.contains("mediaFolderBookmarks")) {
                preferences_.general.mediaFolderBookmarks.clear();
                for (const auto& bm : gen["mediaFolderBookmarks"]) {
                    preferences_.general.mediaFolderBookmarks.push_back(bm.get<std::string>());
                }
            }
        }

        // Load performance preferences
        if (root.contains("performance")) {
            const auto& perf = root["performance"];
            if (perf.contains("videoBufferFrames")) {
                preferences_.performance.videoBufferFrames = perf["videoBufferFrames"];
            }
            if (perf.contains("videoDecodeThreads")) {
                preferences_.performance.videoDecodeThreads = perf["videoDecodeThreads"];
            }
            if (perf.contains("enableHardwareDecoding")) {
                preferences_.performance.enableHardwareDecoding = perf["enableHardwareDecoding"];
            }
            if (perf.contains("enableVSync")) {
                preferences_.performance.enableVSync = perf["enableVSync"];
            }
            if (perf.contains("targetFrameRate")) {
                preferences_.performance.targetFrameRate = perf["targetFrameRate"];
            }
            if (perf.contains("textureQuality")) {
                preferences_.performance.textureQuality = perf["textureQuality"];
            }
            if (perf.contains("enableMipmaps")) {
                preferences_.performance.enableMipmaps = perf["enableMipmaps"];
            }
            if (perf.contains("audioBufferSize")) {
                preferences_.performance.audioBufferSize = perf["audioBufferSize"];
            }
            if (perf.contains("audioSampleRate")) {
                preferences_.performance.audioSampleRate = perf["audioSampleRate"];
            }
            if (perf.contains("outputBufferCount")) {
                preferences_.performance.outputBufferCount = perf["outputBufferCount"];
            }
            if (perf.contains("enableAdaptiveSync")) {
                preferences_.performance.enableAdaptiveSync = perf["enableAdaptiveSync"];
            }
        }

        savedPreferences_ = preferences_;
        modified_ = false;

        Logger::info("Loaded preferences from {}", path);
        return Result<void, Error>::success();

    } catch (const nlohmann::json::exception& e) {
        return Result<void, Error>::error(
            Error("Failed to parse preferences: " + std::string(e.what()))
        );
    }
}

Result<void, Error> PreferencesDialog::saveToFile(const std::string& path) const {
    nlohmann::json root;
    root["version"] = "1.0";

    // Save general preferences
    nlohmann::json gen;
    gen["defaultProjectPath"] = preferences_.general.defaultProjectPath;
    gen["defaultMeshResolution"] = preferences_.general.defaultMeshResolution;
    gen["openLastProjectOnStartup"] = preferences_.general.openLastProjectOnStartup;
    gen["recentProjectsMaxCount"] = preferences_.general.recentProjectsMaxCount;
    gen["autoSaveEnabled"] = preferences_.general.autoSaveEnabled;
    gen["autoSaveIntervalMinutes"] = preferences_.general.autoSaveIntervalMinutes;
    gen["createBackupOnSave"] = preferences_.general.createBackupOnSave;
    gen["showWelcomeDialog"] = preferences_.general.showWelcomeDialog;
    gen["confirmOnExit"] = preferences_.general.confirmOnExit;
    gen["rememberWindowLayout"] = preferences_.general.rememberWindowLayout;
    gen["themeIndex"] = preferences_.general.themeIndex;
    gen["uiMode"] = preferences_.general.uiMode;
    // Media Library settings
    gen["defaultMediaFolder"] = preferences_.general.defaultMediaFolder;
    gen["autoLoadMediaOnStartup"] = preferences_.general.autoLoadMediaOnStartup;
    gen["recursiveFolderScan"] = preferences_.general.recursiveFolderScan;
    gen["mediaFolderBookmarks"] = preferences_.general.mediaFolderBookmarks;
    root["general"] = gen;

    // Save performance preferences
    nlohmann::json perf;
    perf["videoBufferFrames"] = preferences_.performance.videoBufferFrames;
    perf["videoDecodeThreads"] = preferences_.performance.videoDecodeThreads;
    perf["enableHardwareDecoding"] = preferences_.performance.enableHardwareDecoding;
    perf["enableVSync"] = preferences_.performance.enableVSync;
    perf["targetFrameRate"] = preferences_.performance.targetFrameRate;
    perf["textureQuality"] = preferences_.performance.textureQuality;
    perf["enableMipmaps"] = preferences_.performance.enableMipmaps;
    perf["audioBufferSize"] = preferences_.performance.audioBufferSize;
    perf["audioSampleRate"] = preferences_.performance.audioSampleRate;
    perf["outputBufferCount"] = preferences_.performance.outputBufferCount;
    perf["enableAdaptiveSync"] = preferences_.performance.enableAdaptiveSync;
    root["performance"] = perf;

    std::string json = root.dump(2);
    auto result = FileSystem::writeFile(path, json);

    if (result) {
        Logger::info("Saved preferences to {}", path);
    }

    return result;
}

void PreferencesDialog::resetToDefaults() {
    preferences_ = AppPreferences();
    modified_ = true;
    Logger::info("Preferences reset to defaults");
}

} // namespace quadmapper
