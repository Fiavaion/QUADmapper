#pragma once

#ifdef __APPLE__

#include <string>

namespace quadmapper {

// Hands a file, a folder or a URL to macOS's `open`, which shows it with its default
// program; with revealInFinder, Finder opens the containing folder with the file selected.
// The target is passed as one argument, never through a shell.
void macOpen(const std::string& target, bool revealInFinder = false);

}  // namespace quadmapper

#endif
