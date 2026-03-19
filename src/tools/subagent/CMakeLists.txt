add_library(subagent STATIC subagent.c)
target_include_directories(subagent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(subagent PUBLIC core agent pthread)
