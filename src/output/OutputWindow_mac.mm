#include "OutputWindowMac.hpp"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>

namespace quadmapper {

void macCoverMenuBar(GLFWwindow* window, bool cover) {
    NSWindow* native = glfwGetCocoaWindow(window);
    [native setLevel:cover ? NSMainMenuWindowLevel + 1 : NSNormalWindowLevel];
}

}  // namespace quadmapper
