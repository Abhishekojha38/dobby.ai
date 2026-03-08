add_library(provider_openai STATIC openai.c)
target_link_libraries(provider_openai PUBLIC core http)
target_include_directories(provider_openai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
