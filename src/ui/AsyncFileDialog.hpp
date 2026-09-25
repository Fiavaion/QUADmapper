#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace quadmapper {

// A native Windows file or folder dialog that does not block rendering (QM-088).
//
// The Win32 dialogs run their own modal message loop; called on the render thread
// they freeze every projector for as long as they are open. Here the dialog runs on
// a worker thread with its own COM apartment, and the render loop polls
// takeResult() each frame. The dialog has no owner window on purpose: an owner
// created by the render thread would attach the two threads' input queues and be
// disabled for the dialog's lifetime, which is exactly the coupling this avoids.
class AsyncFileDialog {
public:
    enum class Kind { OpenFile, OpenFiles, SaveFile, PickFolder };

    struct Request {
        Kind kind = Kind::OpenFile;
        std::string title;
        std::string filter;       // Win32 filter "Name\0*.a;*.b\0...\0" (unused for folders)
        std::string defaultExt;   // SaveFile: extension appended when none is typed
        std::string initialPath;  // pre-filled file name / starting location
    };

    AsyncFileDialog() = default;
    ~AsyncFileDialog();  // cancels a dialog that is still open, then joins

    AsyncFileDialog(const AsyncFileDialog&) = delete;
    AsyncFileDialog& operator=(const AsyncFileDialog&) = delete;

    // Show the dialog. Returns false (and ignores the request) while a previous
    // dialog's result has not been taken yet, or where native dialogs are unsupported.
    bool open(Request request);

    // True from open() until takeResult() hands the result over.
    bool isBusy() const { return worker_.joinable(); }

    // Once the dialog has closed, moves the chosen paths into out (empty when
    // cancelled) and returns true — exactly once per open(). False while still open.
    bool takeResult(std::vector<std::string>& out);

private:
    void run(const Request& request);

    std::thread worker_;
    std::atomic<bool> done_{false};
    std::atomic<unsigned long> workerThreadId_{0};
    std::vector<std::string> result_;  // written by the worker before done_ is set
};

}  // namespace quadmapper
