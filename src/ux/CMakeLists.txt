# UX library
add_library(ux STATIC
    slash_commands.c
    typing.c
)

target_include_directories(ux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ux PUBLIC core agent pthread)
