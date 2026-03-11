add_library(channels STATIC channel.c)
target_include_directories(channels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(channels bus session agent core pthread)

add_subdirectory(whatsapp)
