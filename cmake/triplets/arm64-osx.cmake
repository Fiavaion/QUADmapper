# vcpkg's arm64-osx triplet with a minimum macOS version (ADR-062, packaging).
# Without VCPKG_OSX_DEPLOYMENT_TARGET the libraries are built for the macOS of the build
# machine only, and the app would not start on anything older. Keep the version equal to
# CMAKE_OSX_DEPLOYMENT_TARGET in CMakePresets.json (mac-base).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.0)
