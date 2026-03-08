# Shell tool
add_library(shell STATIC
    shell_tool.c
)

target_include_directories(shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shell PUBLIC core agent security)
