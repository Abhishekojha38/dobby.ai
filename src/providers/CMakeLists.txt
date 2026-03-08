add_library(providers STATIC http_client.c registry.c)
target_link_libraries(providers core CURL::libcurl)
# Alias so sub-directories can link http
add_library(http ALIAS providers)
