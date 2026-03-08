# Memory library
add_library(memory STATIC
    md_memory.c
)

target_include_directories(memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memory PUBLIC core agent)
