add_library(v6ipc STATIC
    src/protocol.cpp
    src/transport.cpp
    src/commands.cpp
)

target_include_directories(v6ipc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(v6ipc
    PUBLIC
        v6utils
)

if(WIN32)
    target_link_libraries(v6ipc PRIVATE ws2_32)
endif()
