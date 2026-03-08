# tests/CMakeLists.txt — Dobby unit test suite
cmake_minimum_required(VERSION 3.16)

enable_testing()

# ── Email parsing unit tests ─────────────────────────────────────────────────
add_executable(test_email_parsing
    email/test_email_parsing.c
)
target_include_directories(test_email_parsing PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/channels/email
    ${CMAKE_SOURCE_DIR}/src/core
)
target_compile_definitions(test_email_parsing PRIVATE _GNU_SOURCE)
target_compile_options(test_email_parsing PRIVATE -Wall -Wextra -std=c17)

add_test(NAME email_parsing
    COMMAND test_email_parsing
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Allowlist unit tests ──────────────────────────────────────────────────────
add_executable(test_allowlist
    unit/test_allowlist.c
)
target_include_directories(test_allowlist PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/security
    ${CMAKE_SOURCE_DIR}/src/core
)
target_compile_definitions(test_allowlist PRIVATE _GNU_SOURCE)
target_compile_options(test_allowlist PRIVATE -Wall -Wextra -std=c17)
target_link_libraries(test_allowlist security core)

add_test(NAME allowlist
    COMMAND test_allowlist
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Config parser unit tests ──────────────────────────────────────────────────
add_executable(test_config
    unit/test_config.c
)
target_include_directories(test_config PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
)
target_compile_definitions(test_config PRIVATE _GNU_SOURCE)
target_compile_options(test_config PRIVATE -Wall -Wextra -std=c17)
target_link_libraries(test_config core)

add_test(NAME config
    COMMAND test_config
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── History unit tests ────────────────────────────────────────────────────────
add_executable(test_history
    unit/test_history.c
)
target_include_directories(test_history PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/history
    ${CMAKE_SOURCE_DIR}/src/core
)
target_compile_definitions(test_history PRIVATE _GNU_SOURCE)
target_compile_options(test_history PRIVATE -Wall -Wextra -std=c17)
target_link_libraries(test_history history core)

add_test(NAME history
    COMMAND test_history
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
