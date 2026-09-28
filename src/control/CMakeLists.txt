# External control (ADR-048): MIDI input via WinMM, OSC 1.0 input and feedback via
# Winsock UDP, the control-target registry and dispatcher. Depends on core only.

add_library(quadmapper_control STATIC
    ControlEvent.hpp
    ControlEventQueue.hpp
    ControlEventQueue.cpp
    OscParser.hpp
    OscParser.cpp
    OscWriter.hpp
    OscWriter.cpp
    OscPattern.hpp
    OscPattern.cpp
    OscFeedback.hpp
    OscFeedback.cpp
    OscServer.hpp
    OscServer.cpp
    MidiDecode.hpp
    MidiDecode.cpp
    MidiInput.hpp
    MidiInput.cpp
    MidiMapping.hpp
    ControlRegistry.hpp
    ControlRegistry.cpp
    ControlDispatcher.hpp
    ControlDispatcher.cpp
    ExternalControl.hpp
    ExternalControl.cpp
)

target_include_directories(quadmapper_control
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_control
    PUBLIC
        quadmapper_core
    PRIVATE
        winmm      # Windows system lib (MIDI input); no vcpkg dependency
        ws2_32     # Windows system lib (Winsock UDP); no vcpkg dependency
)

set_target_properties(quadmapper_control PROPERTIES
    FOLDER "Libraries"
)
