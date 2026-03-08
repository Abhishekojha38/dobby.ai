add_library(session STATIC session.c)
target_include_directories(session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(session PUBLIC core agent pthread)
