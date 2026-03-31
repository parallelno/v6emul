add_executable(test_client WIN32 main.cpp)

target_link_libraries(test_client PRIVATE v6ipc ws2_32)
