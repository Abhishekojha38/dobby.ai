# File operations tool
add_library(file_ops STATIC
    file_tool.c
)

target_include_directories(file_ops PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(file_ops PUBLIC core agent security)
