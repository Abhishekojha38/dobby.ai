add_executable(dobby main.c)

target_link_libraries(dobby
    core
    agent
    providers
    ollama
    provider_openai
    memory
    history
    security
    shell
    file_ops
    scheduler
    skills
    serial
    heartbeat
    ux
    bus
    session
    channels
    email_channel
    email_tool
    whatsapp_channel
    whatsapp_tool
    gateway
    CURL::libcurl
    ${DL_LIBRARY}
    pthread
    m
)

if(READLINE_LIBRARY)
    target_link_libraries(dobby ${READLINE_LIBRARY})
    target_compile_definitions(dobby PRIVATE HAS_READLINE)
endif()
