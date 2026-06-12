# Integration tests - cross-component functionality

# Rendering pipeline integration
add_quadmapper_test(test_integration_rendering
    RenderingPipelineTest.cpp
)
target_link_libraries(test_integration_rendering PRIVATE
    quadmapper_renderer
    quadmapper_scene
    quadmapper_core
)

# Project save/load integration
add_quadmapper_test(test_integration_project
    ProjectIntegrationTest.cpp
)
target_link_libraries(test_integration_project PRIVATE
    quadmapper_project
    quadmapper_scene
    quadmapper_media
    quadmapper_core
)
