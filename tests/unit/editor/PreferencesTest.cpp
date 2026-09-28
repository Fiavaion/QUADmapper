// The application's preferences file (editor/Preferences.hpp, ADR-051 D2): every
// setting round-trips, bad values and settings this version dropped keep the defaults.

#include "editor/Preferences.hpp"
#include "core/UUID.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace quadmapper;

// ---- Preferences (AppPreferences "control") ------------------------------------

TEST(ControlPreferencesTest, DefaultsAreOffPort8000AllInterfaces) {
    const ControlPreferences prefs;
    EXPECT_FALSE(prefs.oscEnabled);
    EXPECT_EQ(prefs.oscPort, 8000);
    EXPECT_EQ(prefs.oscBindAddress, "0.0.0.0");
    EXPECT_TRUE(prefs.midiDevices.empty());
}

TEST(ControlPreferencesTest, RoundTripsThroughThePreferencesFile) {
    const auto file = std::filesystem::temp_directory_path() /
                      ("qm_control_prefs_" + UUID::generate().toString() + ".json");
    {
        AppPreferences prefs;
        ControlPreferences& c = prefs.control;
        c.oscEnabled = true;
        c.oscPort = 9001;
        c.oscBindAddress = "127.0.0.1";
        c.midiDevices = {"APC mini", "nanoKONTROL2"};
        ASSERT_TRUE(prefs.saveToFile(file.string()));
    }
    AppPreferences loaded;
    ASSERT_TRUE(loaded.loadFromFile(file.string()));
    const ControlPreferences& c = loaded.control;
    EXPECT_TRUE(c.oscEnabled);
    EXPECT_EQ(c.oscPort, 9001);
    EXPECT_EQ(c.oscBindAddress, "127.0.0.1");
    EXPECT_EQ(c.midiDevices, (std::vector<std::string>{"APC mini", "nanoKONTROL2"}));
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// The audio output device is a machine preference beside the control ones (ADR-050).
TEST(AudioPreferencesTest, OutputDeviceRoundTripsAndDefaultsToTheSystemDevice) {
    EXPECT_TRUE(PerformancePreferences{}.audioDevice.empty());  // empty = system default
    const auto file = std::filesystem::temp_directory_path() /
                      ("qm_audio_prefs_" + UUID::generate().toString() + ".json");
    {
        AppPreferences prefs;
        prefs.performance.audioDevice = "Speakers (USB Audio)";
        ASSERT_TRUE(prefs.saveToFile(file.string()));
    }
    AppPreferences loaded;
    ASSERT_TRUE(loaded.loadFromFile(file.string()));
    EXPECT_EQ(loaded.performance.audioDevice, "Speakers (USB Audio)");
    {
        std::ofstream out(file, std::ios::binary);
        out << R"({"version": "1.0", "performance": {"audioDevice": 7}})";
    }
    AppPreferences odd;
    ASSERT_TRUE(odd.loadFromFile(file.string()));
    EXPECT_TRUE(odd.performance.audioDevice.empty());
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(ControlPreferencesTest, OldFilesAndBadValuesKeepDefaults) {
    const auto file = std::filesystem::temp_directory_path() /
                      ("qm_control_prefs_bad_" + UUID::generate().toString() + ".json");
    for (const char* text :
         {R"({"version": "1.0"})",
          R"({"version": "1.0", "control": {"oscEnabled": "yes", "oscPort": 70000,
               "oscBindAddress": 5, "midiDevices": [1, "Pad"]}})"}) {
        SCOPED_TRACE(text);
        {
            std::ofstream out(file, std::ios::binary);
            ASSERT_TRUE(out.good());
            out << text;
        }
        AppPreferences prefs;
        ASSERT_TRUE(prefs.loadFromFile(file.string()));
        const ControlPreferences& c = prefs.control;
        EXPECT_FALSE(c.oscEnabled);
        EXPECT_EQ(c.oscPort, 8000);
        EXPECT_EQ(c.oscBindAddress, "0.0.0.0");
        EXPECT_TRUE(c.midiDevices.empty() || c.midiDevices == std::vector<std::string>{"Pad"});
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(ControlPreferencesTest, FeedbackSettingsDefaultRoundTripAndRejectBadValues) {
    const ControlPreferences defaults;
    EXPECT_FALSE(defaults.oscFeedbackEnabled);
    EXPECT_TRUE(defaults.oscFeedbackHost.empty()) << "empty = reply to the sender";
    EXPECT_EQ(defaults.oscFeedbackPort, 9000);
    EXPECT_FALSE(defaults.oscFeedbackUseIds);

    const auto file = std::filesystem::temp_directory_path() /
                      ("qm_control_feedback_" + UUID::generate().toString() + ".json");
    {
        AppPreferences prefs;
        ControlPreferences& c = prefs.control;
        c.oscFeedbackEnabled = true;
        c.oscFeedbackHost = "192.168.1.20";
        c.oscFeedbackPort = 0;
        c.oscFeedbackUseIds = true;
        ASSERT_TRUE(prefs.saveToFile(file.string()));
    }
    {
        AppPreferences loaded;
        ASSERT_TRUE(loaded.loadFromFile(file.string()));
        const ControlPreferences& c = loaded.control;
        EXPECT_TRUE(c.oscFeedbackEnabled);
        EXPECT_EQ(c.oscFeedbackHost, "192.168.1.20");
        EXPECT_EQ(c.oscFeedbackPort, 0);
        EXPECT_TRUE(c.oscFeedbackUseIds);
    }
    {
        std::ofstream out(file, std::ios::binary);
        out << R"({"version": "1.0", "control": {"oscFeedbackEnabled": 1, "oscFeedbackHost": [],
                   "oscFeedbackPort": 65536, "oscFeedbackUseIds": "no"}})";
    }
    AppPreferences odd;
    ASSERT_TRUE(odd.loadFromFile(file.string()));
    const ControlPreferences& c = odd.control;
    EXPECT_FALSE(c.oscFeedbackEnabled);
    EXPECT_TRUE(c.oscFeedbackHost.empty());
    EXPECT_EQ(c.oscFeedbackPort, 9000);
    EXPECT_FALSE(c.oscFeedbackUseIds);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// ---- Preferences file (TD-28): only settings the app honours ----------------------

namespace {

struct TempPrefsFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("qm_prefs_" + UUID::generate().toString() + ".json");
    ~TempPrefsFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    void write(const char* text) const {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }
    nlohmann::json read() const {
        std::ifstream in(path, std::ios::binary);
        return nlohmann::json::parse(in);
    }
};

}  // namespace

TEST(PreferencesFileTest, EverySettingRoundTrips) {
    TempPrefsFile file;
    {
        AppPreferences p;
        p.general.defaultMediaFolder = "D:/show/media";
        p.general.autoLoadMediaOnStartup = false;
        p.general.recursiveFolderScan = true;
        p.general.autoSaveEnabled = false;
        p.general.autoSaveIntervalMinutes = 12;
        p.general.uiMode = 1;
        p.general.sceneFadeSeconds = 1.25f;
        p.performance.audioDevice = "Speakers (USB Audio)";
        p.control.oscEnabled = true;
        p.control.oscPort = 9100;
        p.control.oscBindAddress = "127.0.0.1";
        p.control.midiDevices = {"APC mini"};
        p.control.oscFeedbackEnabled = true;
        p.control.oscFeedbackHost = "10.0.0.5";
        p.control.oscFeedbackPort = 9001;
        p.control.oscFeedbackUseIds = true;
        ASSERT_TRUE(p.saveToFile(file.path.string()));
    }
    AppPreferences p;
    ASSERT_TRUE(p.loadFromFile(file.path.string()));
    EXPECT_EQ(p.general.defaultMediaFolder, "D:/show/media");
    EXPECT_FALSE(p.general.autoLoadMediaOnStartup);
    EXPECT_TRUE(p.general.recursiveFolderScan);
    EXPECT_FALSE(p.general.autoSaveEnabled);
    EXPECT_EQ(p.general.autoSaveIntervalMinutes, 12);
    EXPECT_EQ(p.general.uiMode, 1);
    EXPECT_FLOAT_EQ(p.general.sceneFadeSeconds, 1.25f);
    EXPECT_EQ(p.performance.audioDevice, "Speakers (USB Audio)");
    EXPECT_TRUE(p.control.oscEnabled);
    EXPECT_EQ(p.control.oscPort, 9100);
    EXPECT_EQ(p.control.oscBindAddress, "127.0.0.1");
    EXPECT_EQ(p.control.midiDevices, std::vector<std::string>{"APC mini"});
    EXPECT_TRUE(p.control.oscFeedbackEnabled);
    EXPECT_EQ(p.control.oscFeedbackHost, "10.0.0.5");
    EXPECT_EQ(p.control.oscFeedbackPort, 9001);
    EXPECT_TRUE(p.control.oscFeedbackUseIds);
}

TEST(PreferencesFileTest, DefaultsAreTheShippedBehaviour) {
    const AppPreferences p;
    EXPECT_TRUE(p.general.defaultMediaFolder.empty());
    EXPECT_TRUE(p.general.autoLoadMediaOnStartup);
    EXPECT_FALSE(p.general.recursiveFolderScan);
    EXPECT_TRUE(p.general.autoSaveEnabled);
    EXPECT_EQ(p.general.autoSaveIntervalMinutes, 1);  // ADR-046: at most a minute lost
    EXPECT_EQ(p.general.uiMode, 0);                   // ADR-042: Simple
    EXPECT_EQ(p.general.sceneFadeSeconds, 0.0f);      // ADR-047 §5: takes cut
    EXPECT_TRUE(p.performance.audioDevice.empty());
}

// A file written by an older version carries settings nothing honoured: they load
// without error, are ignored, and are gone from the next save.
TEST(PreferencesFileTest, SettingsThisVersionDroppedAreIgnoredAndNotWrittenBack) {
    TempPrefsFile file;
    file.write(R"({"version": "1.0",
        "general": {"defaultProjectPath": "C:/p", "defaultMeshResolution": 16,
                    "openLastProjectOnStartup": false, "recentProjectsMaxCount": 5,
                    "mediaFolderBookmarks": ["C:/a"], "createBackupOnSave": false,
                    "showWelcomeDialog": false, "confirmOnExit": false,
                    "rememberWindowLayout": false, "themeIndex": 1,
                    "autoSaveIntervalMinutes": 5, "uiMode": 1, "defaultMediaFolder": "E:/clips"},
        "performance": {"videoBufferFrames": 60, "videoDecodeThreads": 4,
                        "enableHardwareDecoding": false, "enableVSync": false,
                        "targetFrameRate": 30, "textureQuality": 0, "enableMipmaps": false,
                        "audioBufferSize": 256, "audioSampleRate": 44100,
                        "outputBufferCount": 3, "enableAdaptiveSync": true,
                        "audioDevice": "Line Out"},
        "control": {"oscEnabled": true}})");
    AppPreferences p;
    ASSERT_TRUE(p.loadFromFile(file.path.string()));
    EXPECT_EQ(p.general.autoSaveIntervalMinutes, 5);
    EXPECT_EQ(p.general.uiMode, 1);
    EXPECT_EQ(p.general.defaultMediaFolder, "E:/clips");
    EXPECT_EQ(p.performance.audioDevice, "Line Out");
    EXPECT_TRUE(p.control.oscEnabled);

    ASSERT_TRUE(p.saveToFile(file.path.string()));
    const nlohmann::json saved = file.read();
    std::vector<std::string> keys;
    for (const char* section : {"general", "performance", "control"}) {
        ASSERT_TRUE(saved.contains(section)) << section;
        for (const auto& item : saved[section].items()) keys.push_back(std::string(section) + "." + item.key());
    }
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<std::string>{
                        "control.midiDevices", "control.oscBindAddress", "control.oscEnabled",
                        "control.oscFeedbackEnabled", "control.oscFeedbackHost", "control.oscFeedbackPort",
                        "control.oscFeedbackUseIds", "control.oscPort",
                        "general.autoLoadMediaOnStartup", "general.autoSaveEnabled",
                        "general.autoSaveIntervalMinutes", "general.defaultMediaFolder",
                        "general.recursiveFolderScan", "general.sceneFadeSeconds", "general.uiMode",
                        "performance.audioDevice"}));
}

TEST(PreferencesFileTest, WrongTypesAndOutOfRangeValuesKeepDefaults) {
    TempPrefsFile file;
    for (const char* text :
         {R"({"version": "1.0", "general": {"autoSaveIntervalMinutes": 0, "uiMode": 2,
               "autoSaveEnabled": "no", "defaultMediaFolder": 3, "recursiveFolderScan": 1,
               "autoLoadMediaOnStartup": null}})",
          R"({"version": "1.0", "general": {"autoSaveIntervalMinutes": 31, "uiMode": -1}})",
          R"({"version": "1.0", "general": {"autoSaveIntervalMinutes": "5", "uiMode": 1.5}})",
          R"({"version": "1.0", "general": [1, 2], "performance": "x", "control": 7})"}) {
        SCOPED_TRACE(text);
        file.write(text);
        AppPreferences prefs;
        ASSERT_TRUE(prefs.loadFromFile(file.path.string()));
        const GeneralPreferences& g = prefs.general;
        EXPECT_EQ(g.autoSaveIntervalMinutes, 1);
        EXPECT_EQ(g.uiMode, 0);
        EXPECT_TRUE(g.autoSaveEnabled);
        EXPECT_TRUE(g.defaultMediaFolder.empty());
        EXPECT_FALSE(g.recursiveFolderScan);
        EXPECT_TRUE(g.autoLoadMediaOnStartup);
    }
}

// The scene take fade default (ADR-047 §5): seconds, 0 to MAX_SCENE_FADE_SECONDS, an
// integer or a fraction; anything else keeps the current value.
TEST(PreferencesFileTest, SceneFadeDefaultRoundTripsAndRejectsBadValues) {
    TempPrefsFile file;
    file.write(R"({"version": "1.0", "general": {"sceneFadeSeconds": 2}})");
    AppPreferences prefs;
    ASSERT_TRUE(prefs.loadFromFile(file.path.string()));
    EXPECT_FLOAT_EQ(prefs.general.sceneFadeSeconds, 2.0f);
    prefs.general.sceneFadeSeconds = 0.5f;
    ASSERT_TRUE(prefs.saveToFile(file.path.string()));
    AppPreferences loaded;
    ASSERT_TRUE(loaded.loadFromFile(file.path.string()));
    EXPECT_FLOAT_EQ(loaded.general.sceneFadeSeconds, 0.5f);

    for (const char* text : {R"({"version": "1.0", "general": {"sceneFadeSeconds": -0.5}})",
                             R"({"version": "1.0", "general": {"sceneFadeSeconds": 60.5}})",
                             R"({"version": "1.0", "general": {"sceneFadeSeconds": "1"}})",
                             R"({"version": "1.0", "general": {"sceneFadeSeconds": null}})",
                             R"({"version": "1.0", "general": {"sceneFadeSeconds": true}})"}) {
        SCOPED_TRACE(text);
        file.write(text);
        AppPreferences d;
        d.general.sceneFadeSeconds = 3.0f;
        ASSERT_TRUE(d.loadFromFile(file.path.string()));
        EXPECT_FLOAT_EQ(d.general.sceneFadeSeconds, 3.0f);
    }
}

TEST(PreferencesFileTest, AFileThatIsNotPreferencesIsRefusedAndChangesNothing) {
    TempPrefsFile file;
    for (const char* text : {"not json", "[1, 2]", R"({"general": {"uiMode": 1}})"}) {
        SCOPED_TRACE(text);
        file.write(text);
        AppPreferences prefs;
        EXPECT_FALSE(prefs.loadFromFile(file.path.string()));
        EXPECT_EQ(prefs.general.uiMode, 0);
    }
    AppPreferences missing;
    EXPECT_FALSE(missing.loadFromFile((file.path.string() + ".absent")));
}

// A user name outside the ANSI code page (TD follow-up): the path stays wide end to
// end, so the file is written where asked and reads back.
TEST(PreferencesFileTest, APathOutsideTheAnsiCodePageWorks) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      (L"qm_prefs_\u00fc\u65e5\u672c\u0416_" +
                                       std::filesystem::path(UUID::generate().toString()).wstring());
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / L"pr\u00e9f\u00e9rences.json";
    {
        AppPreferences prefs;
        prefs.general.defaultMediaFolder = u8"D:/Vid\u00e9os/\u65e5\u672c";
        ASSERT_TRUE(prefs.saveToFile(file));
    }
    EXPECT_TRUE(std::filesystem::exists(file));
    AppPreferences loaded;
    ASSERT_TRUE(loaded.loadFromFile(file));
    EXPECT_EQ(loaded.general.defaultMediaFolder, u8"D:/Vid\u00e9os/\u65e5\u672c");
    // First run: no file yet. Refused, never thrown (its name goes into the message).
    AppPreferences firstRun;
    EXPECT_FALSE(firstRun.loadFromFile(dir / L"\u65e5\u672c.json"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#ifdef _WIN32
TEST(PreferencesFileTest, DefaultPathIsUnderRoamingAppData) {
    // %APPDATA% is the Roaming AppData folder, read wide (no windows.h here: its UUID
    // clashes with quadmapper::UUID).
    wchar_t* appData = nullptr;
    size_t length = 0;
    ASSERT_EQ(_wdupenv_s(&appData, &length, L"APPDATA"), 0);
    ASSERT_NE(appData, nullptr);
    const std::filesystem::path expected = std::filesystem::path(appData) / L"QUADmapper" / L"preferences.json";
    std::free(appData);
    EXPECT_TRUE(std::filesystem::equivalent(AppPreferences::getDefaultConfigPath().parent_path(),
                                            expected.parent_path()));
    EXPECT_EQ(AppPreferences::getDefaultConfigPath().filename(), expected.filename());
}
#endif
