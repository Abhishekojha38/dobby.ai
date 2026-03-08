add_library(bus STATIC bus.c agent_worker.c)
target_include_directories(bus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bus PUBLIC core agent session pthread)
