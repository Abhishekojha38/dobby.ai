# Skills system
add_library(skills STATIC
    skills.c
)

target_include_directories(skills PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(skills PUBLIC core agent)
