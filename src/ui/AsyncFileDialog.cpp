#include "AsyncFileDialog.hpp"

#include "core/Logger.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#endif

#include <chrono>
#include <cstdio>

namespace quadmapper {

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
#endif
    worker_.join();
}

bool AsyncFileDialog::open(Request request) {
#ifdef _WIN32
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

    std::vector<std::string> chosen;
    if (request.kind == Kind::PickFolder) {
        BROWSEINFOA bi = {};
        bi.lpszTitle = request.title.c_str();
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (LPITEMIDLIST pidl = SHBrowseForFolderA(&bi)) {
            char path[MAX_PATH] = "";
            if (SHGetPathFromIDListA(pidl, path) && path[0] != '\0') chosen.emplace_back(path);
            CoTaskMemFree(pidl);
        }
    } else {
        // Room for a multi-select list (directory + NUL-separated names).
        std::vector<char> buffer(request.kind == Kind::OpenFiles ? 32768 : MAX_PATH, '\0');
        snprintf(buffer.data(), buffer.size(), "%s", request.initialPath.c_str());

        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;  // see the class comment
        ofn.lpstrFilter = request.filter.empty() ? nullptr : request.filter.c_str();
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrTitle = request.title.c_str();
        ofn.lpstrDefExt = request.defaultExt.empty() ? nullptr : request.defaultExt.c_str();
        // OFN_NOCHANGEDIR: the working directory is process-wide, and the render
        // thread's relative resource lookups depend on it.
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        switch (request.kind) {
            case Kind::OpenFile:  ofn.Flags |= OFN_FILEMUSTEXIST; break;
            case Kind::OpenFiles: ofn.Flags |= OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT; break;
            case Kind::SaveFile:  ofn.Flags |= OFN_OVERWRITEPROMPT; break;
            case Kind::PickFolder: break;
        }

        const BOOL ok = request.kind == Kind::SaveFile ? GetSaveFileNameA(&ofn)
                                                       : GetOpenFileNameA(&ofn);
        if (ok) {
            // One file: the full path. Several: the directory, then each name, each
            // NUL-terminated, ending with an empty string.
            const std::string first = buffer.data();
            const char* next = buffer.data() + first.size() + 1;
            if (request.kind != Kind::OpenFiles || *next == '\0') {
                chosen.push_back(first);
            } else {
                while (*next != '\0') {
                    const std::string name = next;
                    chosen.push_back(first + "\\" + name);
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
#else
    (void)request;
#endif
}

}  // namespace quadmapper
