# Ollama provider
add_library(ollama STATIC
    ollama.c
)

target_include_directories(ollama PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ollama PUBLIC core)
