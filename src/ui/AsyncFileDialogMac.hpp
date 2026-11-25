#pragma once

#ifdef __APPLE__

#include "AsyncFileDialog.hpp"

#include <string>
#include <vector>

namespace quadmapper {

// The macOS half of AsyncFileDialog (AsyncFileDialog_mac.mm, Objective-C++,
// ADR-062).
//
// AppKit panels belong to the main thread. macRunFileDialog is called on the dialog's
// worker thread: it asks the main thread to show the panel without a modal loop, so
// projector frames keep running, and waits for the answer. The chosen paths are UTF-8;
// empty when cancelled. `owner` names the dialog that asked (several can be open at
// once) and is only compared, never used.
std::vector<std::string> macRunFileDialog(const AsyncFileDialog::Request& request, const void* owner);

// Main thread only: cancel that owner's panel, if it is open, and run the main run loop
// for a moment so the answer reaches the waiting worker. Called until the worker is done.
void macCancelFileDialog(const void* owner);

}  // namespace quadmapper

#endif
