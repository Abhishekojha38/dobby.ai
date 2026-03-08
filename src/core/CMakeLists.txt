# Core library
add_library(core STATIC
    types.c
    log.c
    config.c
    cJSON.c
    templates.c
)

target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(core PUBLIC ${DL_LIBRARY} pthread m)
