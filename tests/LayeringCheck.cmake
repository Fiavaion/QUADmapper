# layering_check (ADR-051 §1): core -> modules -> editor -> ui -> app.
# Fails on any include of ui/ or app/ under src/editor, and of app/ under src/ui,
# whether written from src ("ui/...") or relative ("../ui/...").
# Run: cmake -DSOURCE_DIR=<repo>/src -P LayeringCheck.cmake

if(NOT DEFINED SOURCE_DIR OR NOT IS_DIRECTORY "${SOURCE_DIR}/editor" OR NOT IS_DIRECTORY "${SOURCE_DIR}/ui")
    message(FATAL_ERROR "layering_check: SOURCE_DIR must be the src directory (got '${SOURCE_DIR}')")
endif()

set(violations 0)

# check_layer(<layer dir> <forbidden dirs, as a regex alternation>)
function(check_layer layer forbidden)
    file(GLOB_RECURSE files "${SOURCE_DIR}/${layer}/*.hpp" "${SOURCE_DIR}/${layer}/*.h"
                            "${SOURCE_DIR}/${layer}/*.cpp")
    list(LENGTH files count)
    set(found ${violations})
    foreach(file IN LISTS files)
        file(STRINGS "${file}" hits REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<](\\.\\./)*(${forbidden})/")
        foreach(hit IN LISTS hits)
            file(RELATIVE_PATH name "${SOURCE_DIR}" "${file}")
            string(STRIP "${hit}" hit)
            message(SEND_ERROR "layering_check: src/${name}: ${hit}")
            math(EXPR found "${found} + 1")
        endforeach()
    endforeach()
    message(STATUS "layering_check: src/${layer}: ${count} files checked against ${forbidden}")
    set(violations ${found} PARENT_SCOPE)
endfunction()

check_layer(editor "ui|app")
check_layer(ui "app")

if(violations GREATER 0)
    message(FATAL_ERROR "layering_check: ${violations} include(s) break the editor -> ui -> app layering")
endif()
message(STATUS "layering_check: OK")
