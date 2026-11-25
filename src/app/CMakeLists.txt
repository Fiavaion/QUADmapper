# Main application executable

add_executable(QUADmapper
    main.cpp
    Application.hpp
    Application.cpp
    AppActions.cpp
    AiAssist.hpp
    AiAssist.cpp
    GrayCodeScan.hpp
    GrayCodeScan.cpp
    StressHarness.hpp
    StressHarness.cpp
    Bench.hpp
    Bench.cpp
    ProjectSession.hpp
    ProjectSession.cpp
    VideoExport.hpp
    VideoExport.cpp
)

target_link_libraries(QUADmapper
    PRIVATE
        quadmapper_core
        quadmapper_project
        quadmapper_scene
        quadmapper_renderer
        quadmapper_media
        quadmapper_video
        quadmapper_audio
        quadmapper_output
        quadmapper_source
        quadmapper_ai
        quadmapper_control
        quadmapper_editor
        quadmapper_ui
        quadmapper_input
        glad
        glfw
        imgui
)

# Windows-specific settings
if(WIN32)
    # Set as Windows application (no console) in Release builds
    set_target_properties(QUADmapper PROPERTIES
        WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    )

    # The icon and the version resource (Explorer > Properties > Details).
    set(QUADMAPPER_ICON "${CMAKE_CURRENT_SOURCE_DIR}/QUADmapper.ico")
    configure_file(QUADmapper.rc.in ${CMAKE_CURRENT_BINARY_DIR}/QUADmapper.rc @ONLY)
    target_sources(QUADmapper PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/QUADmapper.rc)

    # Merged into the linker's own manifest: long paths, since the ".autosave.qmap",
    # ".tmp" and ".bak" suffixes can take a show's path past 260 characters.
    target_sources(QUADmapper PRIVATE QUADmapper.manifest)

    # The shipped exe names its PDB by file name only, not by the build machine's folder.
    target_link_options(QUADmapper PRIVATE
        $<$<CONFIG:Release>:/PDBALTPATH:$<TARGET_PDB_FILE_NAME:QUADmapper>>)

    # H3 stress harness: working-set query via GetProcessMemoryInfo (psapi).
    # Psapi is a Windows system library — no vcpkg dependency.
    target_link_libraries(QUADmapper PRIVATE Psapi)

    # Copy resources to build directory
    add_custom_command(TARGET QUADmapper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/resources
            $<TARGET_FILE_DIR:QUADmapper>/resources
        COMMENT "Copying resources to build directory"
    )

    # The licence notices ship beside the program (Help > Open-Source Licences); .txt
    # so that every Windows machine has a program to open them with.
    add_custom_command(TARGET QUADmapper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
            $<TARGET_FILE_DIR:QUADmapper>/THIRD-PARTY-NOTICES.txt
        COMMENT "Copying licence notices to build directory"
    )
endif()

set_target_properties(QUADmapper PROPERTIES
    FOLDER "Application"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
