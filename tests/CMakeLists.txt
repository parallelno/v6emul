add_executable(cpu_tests cpu_tests.cpp)
target_link_libraries(cpu_tests PRIVATE v6core v6utils)
add_test(NAME cpu_tests COMMAND cpu_tests)

add_executable(memory_tests memory_tests.cpp)
target_link_libraries(memory_tests PRIVATE v6core v6utils)
add_test(NAME memory_tests COMMAND memory_tests)

add_executable(integration_tests integration_tests.cpp)
target_link_libraries(integration_tests PRIVATE v6core v6utils)
add_test(NAME integration_tests COMMAND integration_tests)

add_executable(determinism_tests determinism_tests.cpp)
target_link_libraries(determinism_tests PRIVATE v6core v6utils)
add_test(NAME determinism_tests COMMAND determinism_tests)

add_executable(ipc_tests ipc_tests.cpp)
target_link_libraries(ipc_tests PRIVATE v6core v6ipc v6utils)
add_test(NAME ipc_tests COMMAND ipc_tests)

add_executable(e2e_tests e2e_tests.cpp)
target_link_libraries(e2e_tests PRIVATE v6core v6ipc v6utils)
add_test(NAME e2e_tests COMMAND e2e_tests)

# Golden tests: run v6emul with test ROM and compare stdout to expected output
set(GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/golden")

add_test(
    NAME golden_test_port
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--rom;${GOLDEN_DIR}/test_port.bin;--halt-exit"
        -DEXPECTED=${GOLDEN_DIR}/test_port.expected
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.cmake
)

add_test(
    NAME golden_test_arith
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--rom;${GOLDEN_DIR}/test_arith.bin;--halt-exit"
        -DEXPECTED=${GOLDEN_DIR}/test_arith.expected
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.cmake
)
