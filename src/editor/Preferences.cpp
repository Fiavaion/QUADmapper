#include "Preferences.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "scene/SceneFade.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <iterator>

#ifdef _WIN32
#include <ShlObj.h>
#include <Windows.h>
#endif

namespace quadmapper {

namespace {

using nlohmann::json;

// Type-checked reads: a missing key, the wrong type or an out-of-range number leaves
// `out` as it was.
void readBool(const json& section, const char* key, bool& out) {
    auto it = section.find(key);
    if (it != section.end() && it->is_boolean()) out = it->get<bool>();
}

void readInt(const json& section, const char* key, int& out, int min, int max) {
    auto it = section.find(key);
    if (it == section.end() || !it->is_number_integer()) return;
    const auto value = it->get<long long>();
    if (value >= min && value <= max) out = static_cast<int>(value);
}

void readFloat(const json& section, const char* key, float& out, float min, float max) {
    auto it = section.find(key);
    if (it == section.end() || !it->is_number()) return;
    const double value = it->get<double>();
    if (value >= min && value <= max) out = static_cast<float>(value);  // NaN fails both
}

void readString(const json& section, const char* key, std::string& out) {
    auto it = section.find(key);
    if (it != section.end() && it->is_string()) out = it->get<std::string>();
}

const json* sectionOf(const json& root, const char* name) {
    auto it = root.find(name);
    return it != root.end() && it->is_object() ? &*it : nullptr;
}

// A path compared as Windows does: ASCII case and the slash direction do not matter.
std::string pathKey(const std::string& path) {
    std::string key = path;
    for (char& c : key) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
}

}  // namespace

bool removeRecentProject(std::vector<std::string>& recent, const std::string& path) {
    const std::string key = pathKey(path);
    const auto it = std::remove_if(recent.begin(), recent.end(),
                                   [&key](const std::string& entry) { return pathKey(entry) == key; });
    const bool found = it != recent.end();
    recent.erase(it, recent.end());
    return found;
}

void addRecentProject(std::vector<std::string>& recent, const std::string& path) {
    if (path.empty()) return;
    removeRecentProject(recent, path);
    recent.insert(recent.begin(), path);
    if (recent.size() > MAX_RECENT_PROJECTS) recent.resize(MAX_RECENT_PROJECTS);
}

std::filesystem::path AppPreferences::getDefaultConfigPath() {
    std::filesystem::path configDir;

#ifdef _WIN32
    wchar_t* appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
        configDir = std::filesystem::path(appDataPath);  // wide: no code-page narrowing
    } else {
        configDir = ".";
    }
    CoTaskMemFree(appDataPath);  // required even when the call fails; null is a no-op
    configDir /= "QUADmapper";
#else
    if (const char* xdgConfig = std::getenv("XDG_CONFIG_HOME")) {
        configDir = xdgConfig;
    } else if (const char* home = std::getenv("HOME")) {
        configDir = std::filesystem::path(home) / ".config";
    } else {
        configDir = ".";
    }
    configDir /= "quadmapper";
#endif

    FileSystem::createDirectory(configDir);
    return configDir / "preferences.json";
}

Result<void, Error> AppPreferences::loadFromFile(const std::filesystem::path& path) {
    auto result = FileSystem::readFile(path);
    if (!result) {
        Logger::warning("Could not load preferences from {}: {}", path.u8string(), result.error().message);
        return Result<void, Error>::error(result.error());
    }

    json root;
    try {
        root = json::parse(result.value());
    } catch (const json::exception& e) {
        Logger::warning("Could not parse preferences {}: {}", path.u8string(), e.what());
        return Result<void, Error>::error(Error("Failed to parse preferences: " + std::string(e.what())));
    }
    if (!root.is_object() || !root.contains("version")) {
        Logger::warning("Preferences {} has no version", path.u8string());
        return Result<void, Error>::error(Error("Missing version in preferences file"));
    }

    // Keys this version no longer has (older files carry many) are ignored.
    if (const json* gen = sectionOf(root, "general")) {
        readString(*gen, "defaultMediaFolder", general.defaultMediaFolder);
        readBool(*gen, "autoLoadMediaOnStartup", general.autoLoadMediaOnStartup);
        readBool(*gen, "recursiveFolderScan", general.recursiveFolderScan);
        readBool(*gen, "autoSaveEnabled", general.autoSaveEnabled);
        readInt(*gen, "autoSaveIntervalMinutes", general.autoSaveIntervalMinutes, MIN_AUTOSAVE_MINUTES,
                MAX_AUTOSAVE_MINUTES);
        readInt(*gen, "uiMode", general.uiMode, 0, 1);
        readBool(*gen, "vertexSnapping", general.vertexSnapping);
        readBool(*gen, "showOtherScenes", general.showOtherScenes);
        readBool(*gen, "testGridOnNewQuads", general.testGridOnNewQuads);
        readBool(*gen, "newGridPointsLinear", general.newGridPointsLinear);
        readBool(*gen, "cropInPercent", general.cropInPercent);
        readFloat(*gen, "sceneFadeSeconds", general.sceneFadeSeconds, 0.0f, MAX_SCENE_FADE_SECONDS);
        int scale = 0;
        readInt(*gen, "uiScalePercent", scale, 0, 400);
        if (scale == 0 || std::find(std::begin(UI_SCALE_CHOICES), std::end(UI_SCALE_CHOICES), scale) !=
                              std::end(UI_SCALE_CHOICES)) {
            general.uiScalePercent = scale;
        }
        readBool(*gen, "aiAssistBeta", general.aiAssistBeta);
    }

    if (const json* perf = sectionOf(root, "performance")) {
        readString(*perf, "audioDevice", performance.audioDevice);
    }

    // External control (ADR-048).
    if (const json* ctl = sectionOf(root, "control")) {
        readBool(*ctl, "oscEnabled", control.oscEnabled);
        readInt(*ctl, "oscPort", control.oscPort, 1, 65535);
        readString(*ctl, "oscBindAddress", control.oscBindAddress);
        auto devices = ctl->find("midiDevices");
        if (devices != ctl->end() && devices->is_array()) {
            control.midiDevices.clear();
            for (const auto& device : *devices) {
                if (device.is_string()) control.midiDevices.push_back(device.get<std::string>());
            }
        }
        readBool(*ctl, "oscFeedbackEnabled", control.oscFeedbackEnabled);
        readString(*ctl, "oscFeedbackHost", control.oscFeedbackHost);
        readInt(*ctl, "oscFeedbackPort", control.oscFeedbackPort, 0, 65535);  // 0 = the sender's port
        readBool(*ctl, "oscFeedbackUseIds", control.oscFeedbackUseIds);
    }

    // Rebound keys: id -> ["Ctrl+Shift+L", ...]. Whether the ids and chords are valid is
    // checked where they are applied (applyKeyBindings).
    if (const json* keys = sectionOf(root, "keyboard")) {
        keyBindings.clear();
        for (const auto& [id, chords] : keys->items()) {
            const bool strings = chords.is_array() &&
                                 std::all_of(chords.begin(), chords.end(),
                                             [](const json& chord) { return chord.is_string(); });
            if (!strings) {
                Logger::warning("Preferences: ignored the keys of \"{}\": not a list of key chords", id);
                continue;
            }
            std::vector<std::string>& texts = keyBindings[id];
            for (const json& chord : chords) texts.push_back(chord.get<std::string>());
        }
    }

    // Recent shows: anything but a list of non-empty strings is skipped, never an error.
    recentProjects.clear();
    auto recent = root.find("recentProjects");
    if (recent != root.end() && recent->is_array()) {
        for (const json& entry : *recent) {
            if (entry.is_string() && !entry.get<std::string>().empty()) {
                const std::string path = entry.get<std::string>();
                const std::string key = pathKey(path);
                const bool repeated = std::any_of(recentProjects.begin(), recentProjects.end(),
                    [&key](const std::string& have) { return pathKey(have) == key; });
                if (!repeated && recentProjects.size() < MAX_RECENT_PROJECTS) recentProjects.push_back(path);
            }
        }
    }

    Logger::info("Loaded preferences from {}", path.u8string());
    return Result<void, Error>::success();
}

Result<void, Error> AppPreferences::saveToFile(const std::filesystem::path& path) const {
    json root;
    root["version"] = "1.0";

    json gen;
    gen["defaultMediaFolder"] = general.defaultMediaFolder;
    gen["autoLoadMediaOnStartup"] = general.autoLoadMediaOnStartup;
    gen["recursiveFolderScan"] = general.recursiveFolderScan;
    gen["autoSaveEnabled"] = general.autoSaveEnabled;
    gen["autoSaveIntervalMinutes"] = general.autoSaveIntervalMinutes;
    gen["uiMode"] = general.uiMode;
    gen["vertexSnapping"] = general.vertexSnapping;
    gen["showOtherScenes"] = general.showOtherScenes;
    gen["testGridOnNewQuads"] = general.testGridOnNewQuads;
    gen["newGridPointsLinear"] = general.newGridPointsLinear;
    gen["cropInPercent"] = general.cropInPercent;
    gen["sceneFadeSeconds"] = general.sceneFadeSeconds;
    gen["uiScalePercent"] = general.uiScalePercent;
    gen["aiAssistBeta"] = general.aiAssistBeta;
    root["general"] = gen;
    root["recentProjects"] = recentProjects;

    json perf;
    perf["audioDevice"] = performance.audioDevice;
    root["performance"] = perf;

    json ctl;
    ctl["oscEnabled"] = control.oscEnabled;
    ctl["oscPort"] = control.oscPort;
    ctl["oscBindAddress"] = control.oscBindAddress;
    ctl["midiDevices"] = control.midiDevices;
    ctl["oscFeedbackEnabled"] = control.oscFeedbackEnabled;
    ctl["oscFeedbackHost"] = control.oscFeedbackHost;
    ctl["oscFeedbackPort"] = control.oscFeedbackPort;
    ctl["oscFeedbackUseIds"] = control.oscFeedbackUseIds;
    root["control"] = ctl;

    json keys = json::object();
    for (const auto& [id, chords] : keyBindings) keys[id] = chords;
    root["keyboard"] = keys;

    // Atomic: a crash or full disk mid-write leaves the previous file, never half of one.
    auto result = FileSystem::writeFileAtomic(path, root.dump(2));
    if (result) {
        Logger::info("Saved preferences to {}", path.u8string());
    }
    return result;
}

}  // namespace quadmapper
