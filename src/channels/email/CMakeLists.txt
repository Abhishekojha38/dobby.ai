add_library(email_channel STATIC email_channel.c)
target_include_directories(email_channel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(email_channel PUBLIC bus channels core CURL::libcurl pthread)
