add_library(email_tool STATIC email_tool.c)
target_include_directories(email_tool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(email_tool PUBLIC email_channel core agent)
