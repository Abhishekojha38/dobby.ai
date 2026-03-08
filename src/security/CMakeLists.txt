# Security library
add_library(security STATIC
    allowlist.c
)

target_include_directories(security PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(security PUBLIC core)
