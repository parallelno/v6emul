add_executable(cpu_tests cpu_tests.cpp)
target_link_libraries(cpu_tests PRIVATE v6core v6utils)
add_test(NAME cpu_tests COMMAND cpu_tests)

add_executable(memory_tests memory_tests.cpp)
target_link_libraries(memory_tests PRIVATE v6core v6utils)
add_test(NAME memory_tests COMMAND memory_tests)
