#include "Preferences.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "scene/SceneFade.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>

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

}  // namespace

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
        return Result<void, Error>::error(Error("Failed to parse preferences: " + std::string(e.what())));
    }
    if (!root.is_object() || !root.contains("version")) {
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
        readFloat(*gen, "sceneFadeSeconds", general.sceneFadeSeconds, 0.0f, MAX_SCENE_FADE_SECONDS);
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
    gen["sceneFadeSeconds"] = general.sceneFadeSeconds;
    root["general"] = gen;

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

    // Atomic: a crash or full disk mid-write leaves the previous file, never half of one.
    auto result = FileSystem::writeFileAtomic(path, root.dump(2));
    if (result) {
        Logger::info("Saved preferences to {}", path.u8string());
    }
    return result;
}

}  // namespace quadmapper
