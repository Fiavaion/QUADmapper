#include "Application.hpp"
#include "../core/FileSystem.hpp"
#include "../core/Logger.hpp"

#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>

// A Windows system library (minidumps); nothing to install or ship.
#pragma comment(lib, "dbghelp.lib")

namespace {

quadmapper::Application* g_app = nullptr;
DWORD g_mainThreadId = 0;
bool g_showMessages = true;  // false for the headless --stress / --bench runs
// Filled at start: the crash filter must not allocate.
wchar_t g_dumpPath[MAX_PATH] = L"";
wchar_t g_crashMessage[1024] = L"";

void showError(const std::wstring& text) {
    if (!g_showMessages) return;
    // Topmost: a fullscreen projector window must not hide it.
    MessageBoxW(nullptr, text.c_str(), L"QUADmapper",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}

std::wstring logFolderNote() {
    const std::string log = quadmapper::Logger::getLogFilePath();
    if (log.empty()) return L"";
    return L"\n\nThe log has the details:\n" + quadmapper::pathFromUtf8(log).wstring();
}

// The emergency save runs on a process that has already faulted, so a second fault in
// it is swallowed: the dump is written by then and the process is about to end.
void emergencySaveAfterCrash() {
    __try {
        g_app->emergencySave();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A crash that is not a C++ exception (access violation, stack overflow, ...): write a
// minidump for the bug report, try the emergency save, tell the user, end the process.
LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;

    if (g_dumpPath[0] != L'\0') {
        HANDLE file = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
                              &exception, nullptr, nullptr);
            CloseHandle(file);
        }
    }
    // Only from the main thread: on any other thread the editor is still being changed
    // under the save. The autosave covers that case.
    if (g_app && GetCurrentThreadId() == g_mainThreadId) emergencySaveAfterCrash();

    if (g_showMessages) {
        MessageBoxW(nullptr, g_crashMessage, L"QUADmapper",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashFilter() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = quadmapper::userLocalDir() / "crashes";
    fs::create_directories(dir, ec);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[64];
    swprintf(name, 64, L"QUADmapper-%04d%02d%02d-%02d%02d%02d.dmp", now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute, now.wSecond);
    const std::wstring path = (dir / name).wstring();
    if (path.size() < MAX_PATH) wcscpy_s(g_dumpPath, path.c_str());
    swprintf(g_crashMessage, 1024,
             L"QUADmapper has stopped because of an internal error.\n\n"
             L"It tried to save a recovery copy of your show. If that worked, you will be "
             L"offered it the next time you start QUADmapper.\n\n"
             L"A crash report was written to:\n%ls",
             dir.c_str());

    g_mainThreadId = GetCurrentThreadId();
    SetUnhandledExceptionFilter(crashFilter);
}

// The command line as UTF-8: the narrow argv loses any character outside the ANSI code
// page, so a show in a folder with such a name could not be opened by double-click.
std::vector<std::string> utf8Arguments() {
    std::vector<std::string> args;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) return args;
    for (int i = 0; i < count; ++i) {
        args.push_back(quadmapper::utf8FromPath(std::filesystem::path(wide[i])));
    }
    LocalFree(wide);
    return args;
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[]) {
#ifdef NDEBUG
    quadmapper::Logger::setLevel(quadmapper::LogLevel::Info);
#else
    quadmapper::Logger::setLevel(quadmapper::LogLevel::Debug);
#endif

    std::vector<std::string> args = utf8Arguments();
    std::vector<char*> argv;
    for (std::string& arg : args) {
        argv.push_back(arg.data());
        // The headless harnesses (StressHarness, Bench) run unattended: no message boxes.
        if (arg.rfind("--stress", 0) == 0 || arg.rfind("--bench", 0) == 0) g_showMessages = false;
    }

    installCrashFilter();

    // Create and run application
    quadmapper::Application app;
    g_app = &app;

    // Crash guard (ADR-046): an escaping C++ exception gets a best-effort emergency
    // save to <project>.crash.qmap (never the real file) before the process exits.
    try {
        if (!app.initialize(static_cast<int>(argv.size()), argv.data())) {
            quadmapper::Logger::error("Application initialization failed");
            showError(L"QUADmapper could not start.\n\n"
                      L"The usual causes are a graphics driver without OpenGL 3.3, or a "
                      L"missing program file." + logFolderNote());
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
    showError(L"QUADmapper has stopped because of an internal error.\n\n"
              L"It tried to save a recovery copy of your show. If that worked, you will be "
              L"offered it the next time you start QUADmapper." + logFolderNote());
    return 2;
}

// Windows entry point for GUI applications (no console window in Release builds)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
    return main(__argc, __argv);
}
