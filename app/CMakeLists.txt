add_executable(v6emul
    main.cpp
    server_mode.cpp
    test_mode.cpp
)

target_link_libraries(v6emul PRIVATE v6core v6ipc v6utils)
target_include_directories(v6emul PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
)
