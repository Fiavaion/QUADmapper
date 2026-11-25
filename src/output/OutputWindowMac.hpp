#pragma once

// The macOS half of a projector window, for OutputWindow.cpp only.

struct GLFWwindow;

namespace quadmapper {

/// Puts the window above the macOS menu bar and the Dock (true), or back at the ordinary
/// window level (false). macOS draws the menu bar on every display, above an ordinary
/// borderless window, and GLFW has no call for the window level.
void macCoverMenuBar(GLFWwindow* window, bool cover);

}  // namespace quadmapper
