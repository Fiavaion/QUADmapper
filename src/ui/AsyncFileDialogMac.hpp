#pragma once

#ifdef __APPLE__

#include "AsyncFileDialog.hpp"

#include <string>
#include <vector>

namespace quadmapper {

// The macOS half of AsyncFileDialog (AsyncFileDialog_mac.mm, the app's only
// Objective-C++ file, ADR-062).
//
// AppKit panels belong to the main thread. macRunFileDialog is called on the dialog's
// worker thread: it asks the main thread to show the panel without a modal loop, so
// projector frames keep running, and waits for the answer. The chosen paths are UTF-8;
// empty when cancelled.
std::vector<std::string> macRunFileDialog(const AsyncFileDialog::Request& request);

// Main thread only: cancel the open panel, if any, and run the main run loop for a
// moment so the answer reaches the waiting worker. Called until the worker is done.
void macCancelFileDialog();

}  // namespace quadmapper

#endif
