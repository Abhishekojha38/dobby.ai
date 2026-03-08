add_library(gateway STATIC gateway.c)
target_include_directories(gateway PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gateway PUBLIC core bus pthread)
