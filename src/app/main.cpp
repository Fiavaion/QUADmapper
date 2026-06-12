#include "Application.hpp"
#include "../core/Logger.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[]) {
    // Initialize logging
    quadmapper::Logger::setLevel(quadmapper::LogLevel::Debug);
    quadmapper::Logger::info("QUADmapper starting...");

    // Create and run application
    quadmapper::Application app;

    if (!app.initialize(argc, argv)) {
        quadmapper::Logger::error("Application initialization failed");
        return 1;
    }

    int result = app.run();

    quadmapper::Logger::info("QUADmapper exiting with code {}", result);
    return result;
}

#ifdef _WIN32
// Windows entry point for GUI applications (no console window in Release builds)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
    return main(__argc, __argv);
}
#endif
