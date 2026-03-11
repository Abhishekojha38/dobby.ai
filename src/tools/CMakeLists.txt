add_library(whatsapp_tool STATIC whatsapp_tool.c)
target_include_directories(whatsapp_tool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(whatsapp_tool whatsapp_channel agent core)
