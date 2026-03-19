cmake_minimum_required(VERSION 3.16)
project(dobby VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# _GNU_SOURCE globally: provides localtime_r, strcasestr, strchrnul, etc.
# Defining it here prevents "redefined" warnings when individual .c files
# also define it (those per-file defines will be cleaned up separately).
add_compile_definitions(_GNU_SOURCE)

add_compile_options(-Wall -Wextra -Wpedantic -Os
                    -ffunction-sections -fdata-sections)
if(APPLE)
    add_link_options(-Wl,-dead_strip)
else()
    add_link_options(-Wl,--gc-sections)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(CURL REQUIRED)

find_library(READLINE_LIBRARY readline)
if(NOT READLINE_LIBRARY)
    message(STATUS "libreadline not found — using fgets fallback")
endif()

find_library(DL_LIBRARY dl)
if(NOT DL_LIBRARY)
    set(DL_LIBRARY "")
endif()

option(BUILD_TESTS "Build unit tests" OFF)

add_subdirectory(src/core)
add_subdirectory(src/agent)
add_subdirectory(src/providers)
add_subdirectory(src/providers/ollama)
add_subdirectory(src/providers/openai)
add_subdirectory(src/memory)
add_subdirectory(src/history)
add_subdirectory(src/security)
add_subdirectory(src/tools/shell)
add_subdirectory(src/tools/file_ops)
add_subdirectory(src/tools/scheduler)
add_subdirectory(src/tools/skills)
add_subdirectory(src/tools/serial)
add_subdirectory(src/tools/subagent)
add_subdirectory(src/heartbeat)
add_subdirectory(src/ux)
add_subdirectory(src/bus)
add_subdirectory(src/session)
add_subdirectory(src/channels)
add_subdirectory(src/channels/email)
add_subdirectory(src/tools/email)
add_subdirectory(src/tools/whatsapp)
add_subdirectory(src/gateway)
add_subdirectory(src/cli)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install targets ──────────────────────────────────────────────────────────
install(TARGETS dobby RUNTIME DESTINATION bin)
install(DIRECTORY skills/    DESTINATION share/dobby/skills)
install(DIRECTORY templates/ DESTINATION share/dobby/templates)
install(DIRECTORY assets/    DESTINATION share/dobby/assets)
install(FILES dobby.conf     DESTINATION share/dobby)
install(FILES allowlist.conf DESTINATION share/dobby)
