# Main application executable

add_executable(QUADmapper
    main.cpp
    Application.hpp
    Application.cpp
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

    # Copy resources to build directory
    add_custom_command(TARGET QUADmapper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/resources
            $<TARGET_FILE_DIR:QUADmapper>/resources
        COMMENT "Copying resources to build directory"
    )
endif()

set_target_properties(QUADmapper PROPERTIES
    FOLDER "Application"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
