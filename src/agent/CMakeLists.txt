# Agent library
add_library(agent STATIC
    agent.c
    tool_registry.c
)

target_include_directories(agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent PUBLIC core)
