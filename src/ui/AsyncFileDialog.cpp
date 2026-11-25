#include "AsyncFileDialog.hpp"

#include "AsyncFileDialogMac.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace quadmapper {

#ifdef _WIN32
namespace {

// Request strings are UTF-8 and the W dialogs take UTF-16 (the A dialogs would turn
// any name outside the ANSI code page into '?'). Length-counted, so the filter's
// embedded NULs survive.
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = static_cast<int>(utf8.size());
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, out.data(), len);
    return out;
}

}  // namespace
#endif

AsyncFileDialog::~AsyncFileDialog() {
    if (!worker_.joinable()) return;
#ifdef _WIN32
    // The dialog is modal to its own thread only: ask it to cancel (WM_CLOSE is the
    // dialog's Cancel) until the worker finishes. Repeats because the window may not
    // exist yet when shutdown begins.
    while (!done_.load(std::memory_order_acquire)) {
        const DWORD tid = workerThreadId_.load();
        if (tid != 0) {
            EnumThreadWindows(tid, [](HWND hwnd, LPARAM) -> BOOL {
                PostMessageA(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            }, 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#elif defined(__APPLE__)
    // The panel belongs to the main thread, which is the one destroying this object.
    while (!done_.load(std::memory_order_acquire)) macCancelFileDialog();
#endif
    worker_.join();
}

bool AsyncFileDialog::open(Request request) {
#if defined(_WIN32) || defined(__APPLE__)
    if (worker_.joinable()) return false;
    done_.store(false);
    result_.clear();
    worker_ = std::thread([this, request = std::move(request)]() { run(request); });
    return true;
#else
    (void)request;
    return false;
#endif
}

bool AsyncFileDialog::takeResult(std::vector<std::string>& out) {
    if (!worker_.joinable() || !done_.load(std::memory_order_acquire)) return false;
    worker_.join();
    out = std::move(result_);
    result_.clear();
    return true;
}

void AsyncFileDialog::run(const Request& request) {
#ifdef _WIN32
    workerThreadId_.store(GetCurrentThreadId());
    // The shell dialogs need a single-threaded COM apartment on the thread showing them.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Chosen paths are handed back as UTF-8, like every path string in the app.
    std::vector<std::string> chosen;
    const std::wstring title = widen(request.title);
    if (request.kind == Kind::PickFolder) {
        BROWSEINFOW bi = {};
        bi.lpszTitle = title.c_str();
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (LPITEMIDLIST pidl = SHBrowseForFolderW(&bi)) {
            wchar_t path[MAX_PATH] = L"";
            if (SHGetPathFromIDListW(pidl, path) && path[0] != L'\0') {
                chosen.push_back(utf8FromPath(std::filesystem::path(path)));
            }
            CoTaskMemFree(pidl);
        }
    } else {
        const std::wstring filter = widen(request.filter);
        const std::wstring defaultExt = widen(request.defaultExt);
        // Room for a multi-select list (directory + NUL-separated names). A short buffer
        // made a long path return nothing; now the open or save that follows says why
        // it fails.
        std::vector<wchar_t> buffer(32768, L'\0');
        const std::wstring initialPath = widen(request.initialPath);
        initialPath.copy(buffer.data(), std::min(initialPath.size(), buffer.size() - 1));

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;  // see the class comment
        ofn.lpstrFilter = filter.empty() ? nullptr : filter.c_str();
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrTitle = title.c_str();
        ofn.lpstrDefExt = defaultExt.empty() ? nullptr : defaultExt.c_str();
        // OFN_NOCHANGEDIR: the working directory is process-wide, and the render
        // thread's relative resource lookups depend on it.
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        switch (request.kind) {
            case Kind::OpenFile:  ofn.Flags |= OFN_FILEMUSTEXIST; break;
            case Kind::OpenFiles: ofn.Flags |= OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT; break;
            case Kind::SaveFile:  ofn.Flags |= OFN_OVERWRITEPROMPT; break;
            case Kind::PickFolder: break;
        }

        const BOOL ok = request.kind == Kind::SaveFile ? GetSaveFileNameW(&ofn)
                                                       : GetOpenFileNameW(&ofn);
        if (ok) {
            // One file: the full path. Several: the directory, then each name, each
            // NUL-terminated, ending with an empty string.
            const std::wstring first = buffer.data();
            const wchar_t* next = buffer.data() + first.size() + 1;
            if (request.kind != Kind::OpenFiles || *next == L'\0') {
                chosen.push_back(utf8FromPath(std::filesystem::path(first)));
            } else {
                while (*next != L'\0') {
                    const std::wstring name = next;
                    chosen.push_back(utf8FromPath(std::filesystem::path(first) / name));
                    next += name.size() + 1;
                }
            }
        } else if (const DWORD err = CommDlgExtendedError(); err != 0) {
            Logger::warning("File dialog '{}' failed (CommDlgExtendedError {})", request.title, err);
        }
    }

    if (SUCCEEDED(com)) CoUninitialize();
    result_ = std::move(chosen);
    done_.store(true, std::memory_order_release);
#elif defined(__APPLE__)
    result_ = macRunFileDialog(request);
    done_.store(true, std::memory_order_release);
#else
    (void)request;
#endif
}

}  // namespace quadmapper
