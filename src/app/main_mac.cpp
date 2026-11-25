// Process start on macOS. main_win.cpp is the Windows one; the two run the same
// sequence (single instance, crash guard, initialize, run, emergency save on an escaped
// exception) with each system's own calls.

#include "Application.hpp"
#include "../core/FileSystem.hpp"
#include "../core/Logger.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace {

quadmapper::Application* g_app = nullptr;
bool g_showMessages = true;  // false for the headless --stress / --bench runs
std::atomic<bool> g_crashEntered{false};

// A system alert that needs no window of ours and shows above a fullscreen projector.
void showAlert(const std::string& text, bool isError) {
    if (!g_showMessages) return;
    CFStringRef message = CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
    if (!message) return;
    CFOptionFlags response = 0;
    CFUserNotificationDisplayAlert(
        0, isError ? kCFUserNotificationStopAlertLevel : kCFUserNotificationNoteAlertLevel, nullptr,
        nullptr, nullptr, CFSTR("QUADmapper"), message, nullptr, nullptr, nullptr, &response);
    CFRelease(message);
}

std::string logFolderNote() {
    const std::string log = quadmapper::Logger::getLogFilePath();
    if (log.empty()) return "";
    return "\n\nThe log has the details:\n" + log;
}

const char* const CRASH_MESSAGE =
    "QUADmapper has stopped because of an internal error.\n\n"
    "It tried to save a recovery copy of your show. If that worked, you will be "
    "offered it the next time you start QUADmapper.";

// std::terminate (a C++ exception that escaped a worker thread or a noexcept function)
// ends in abort(). The emergency save runs only on the main thread: on any other the
// editor is still being changed under the save, and the autosave is the cover. macOS
// writes the crash report itself (Console > Crash Reports), for this and for a fault.
void onAbort(int) {
    if (g_crashEntered.exchange(true)) return;
    if (g_app && pthread_main_np() != 0) g_app->emergencySave();
    showAlert(CRASH_MESSAGE, true);
}

// One QUADmapper per user session: two would share the recovery files, the preferences,
// the OSC port and the projectors. The lock is held for as long as this process lives
// and is released by the system when it ends, however it ends.
bool anotherInstanceRuns(bool wantedToOpenAShow) {
    const std::string lockFile = quadmapper::utf8FromPath(quadmapper::userConfigDir() / "instance.lock");
    const int fd = open(lockFile.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return false;  // no lock file can be made: do not refuse to start over it
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return false;  // ours now; never closed
    close(fd);
    showAlert(wantedToOpenAShow
                  ? "QUADmapper is already running.\n\nOpen the show there with File > Open."
                  : "QUADmapper is already running.",
              false);
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef NDEBUG
    quadmapper::Logger::setLevel(quadmapper::LogLevel::Info);
#else
    quadmapper::Logger::setLevel(quadmapper::LogLevel::Debug);
#endif

    bool opensAShow = false;
    std::optional<quadmapper::StressConfig> stressFlags;
    std::optional<quadmapper::BenchConfig> benchFlags;
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];  // UTF-8 already on macOS
        // The headless harnesses (StressHarness, Bench) run unattended: no alerts.
        // Asked of the same parsers Application uses, so only a real harness flag counts.
        if (quadmapper::parseStressArg(arg.c_str(), stressFlags) ||
            quadmapper::parseBenchArg(arg.c_str(), benchFlags)) {
            g_showMessages = false;
        }
        if (arg.size() > 5 && strcasecmp(arg.c_str() + arg.size() - 5, ".qmap") == 0) opensAShow = true;
    }

    // The harnesses run beside each other and beside the app.
    if (g_showMessages && anotherInstanceRuns(opensAShow)) return 0;

    std::signal(SIGABRT, onAbort);

    quadmapper::Application app;
    g_app = &app;

    // Crash guard (ADR-046): an escaping C++ exception gets a best-effort emergency
    // save to <project>.crash.qmap (never the real file) before the process exits.
    try {
        if (!app.initialize(argc, argv)) {
            quadmapper::Logger::error("Application initialization failed");
            showAlert("QUADmapper could not start.\n\n"
                      "The usual causes are a display that cannot give an OpenGL 3.3 "
                      "context, or a missing program file." + logFolderNote(),
                      true);
            g_app = nullptr;
            return 1;
        }

        int result = app.run();

        quadmapper::Logger::info("QUADmapper exiting with code {}", result);
        g_app = nullptr;
        return result;
    } catch (const std::exception& e) {
        quadmapper::Logger::error("Fatal: unhandled exception: {}", e.what());
    } catch (...) {
        quadmapper::Logger::error("Fatal: unhandled exception of unknown type");
    }
    app.emergencySave();
    g_app = nullptr;
    showAlert(std::string(CRASH_MESSAGE) + logFolderNote(), true);
    return 2;
}
