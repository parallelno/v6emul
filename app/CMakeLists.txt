add_executable(v6emul main.cpp)

target_link_libraries(v6emul PRIVATE v6core v6ipc v6utils)
