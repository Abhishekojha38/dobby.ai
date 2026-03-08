# Serial port tool
add_library(serial STATIC
    serial_tool.c
)

target_include_directories(serial PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(serial PUBLIC core agent)
