add_library(whatsapp_channel STATIC whatsapp_channel.c)
target_include_directories(whatsapp_channel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(whatsapp_channel bus core security pthread)
