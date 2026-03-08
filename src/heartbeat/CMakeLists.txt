# Heartbeat
add_library(heartbeat STATIC
    heartbeat.c
)

target_include_directories(heartbeat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(heartbeat PUBLIC core pthread)
