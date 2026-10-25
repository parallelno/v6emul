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
target_sources(ipc_tests PRIVATE ${CMAKE_SOURCE_DIR}/app/ipc_request.cpp)
target_link_libraries(ipc_tests PRIVATE v6core v6ipc v6utils)
target_include_directories(ipc_tests PRIVATE ${CMAKE_SOURCE_DIR}/app)
add_test(NAME ipc_tests COMMAND ipc_tests)
set_tests_properties(ipc_tests PROPERTIES TIMEOUT 30)

add_executable(e2e_tests e2e_tests.cpp)
target_link_libraries(e2e_tests PRIVATE v6core v6ipc v6utils)
add_test(NAME e2e_tests COMMAND e2e_tests)

add_executable(screenshot_tests
    screenshot_tests.cpp
    ${CMAKE_SOURCE_DIR}/app/screenshot.cpp
)
target_link_libraries(screenshot_tests PRIVATE v6core v6utils)
target_include_directories(screenshot_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/third_party/stb
)
add_test(NAME screenshot_tests COMMAND screenshot_tests)

# Screenshot CLI and end-to-end tests
set(SCREENSHOT_TEST_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/run_screenshot_cli.cmake")
set(SCREENSHOT_E2E_BMP "${CMAKE_CURRENT_BINARY_DIR}/screenshot_e2e.BMP")
set(SCREENSHOT_E2E_512_BMP "${CMAKE_CURRENT_BINARY_DIR}/screenshot_e2e_512.bmp")
set(SCREENSHOT_E2E_512_ROM "${CMAKE_CURRENT_BINARY_DIR}/screenshot_e2e_512.bin")

# MVI A,0x11; OUT 0x02 (enable 512 mode); HLT
string(ASCII 62 17 211 2 118 SCREENSHOT_E2E_512_BYTES)
file(WRITE "${SCREENSHOT_E2E_512_ROM}" "${SCREENSHOT_E2E_512_BYTES}")

add_test(
    NAME screenshot_cli_missing_path
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--screenshot"
        -DEXPECT_EXIT=1
        "-DEXPECT_STDOUT=Required parameter \"screenshot\" or its value was not provided."
        "-DEXPECT_STDOUT2=Parameters:"
        -P ${SCREENSHOT_TEST_SCRIPT}
)

add_test(
    NAME screenshot_cli_bad_extension
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--screenshot;out.png;--run-frames;1"
        -DEXPECT_EXIT=1
        "-DEXPECT_STDERR=unsupported screenshot extension"
        -P ${SCREENSHOT_TEST_SCRIPT}
)

add_test(
    NAME screenshot_cli_requires_stop_condition
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--screenshot;out.bmp"
        -DEXPECT_EXIT=1
        "-DEXPECT_STDERR=--screenshot requires --halt-exit, --run-frames, or --run-cycles"
        -P ${SCREENSHOT_TEST_SCRIPT}
)

file(REMOVE "${SCREENSHOT_E2E_BMP}")
add_test(
    NAME screenshot_e2e_bmp
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--rom;${CMAKE_CURRENT_SOURCE_DIR}/golden/test_port.bin;--load-addr;0;--run-frames;1;--screenshot;${SCREENSHOT_E2E_BMP}"
        -DEXPECT_EXIT=0
        -DEXPECT_FILE=${SCREENSHOT_E2E_BMP}
        -DEXPECT_MAGIC=424d
        -DEXPECT_SIZE=1000
        -DEXPECT_WIDTH=256
        -DEXPECT_HEIGHT=256
        -P ${SCREENSHOT_TEST_SCRIPT}
)

file(REMOVE "${SCREENSHOT_E2E_512_BMP}")
add_test(
    NAME screenshot_e2e_bmp_512_mode
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--rom;${SCREENSHOT_E2E_512_ROM};--load-addr;0;--run-frames;1;--screenshot;${SCREENSHOT_E2E_512_BMP}"
        -DEXPECT_EXIT=0
        -DEXPECT_FILE=${SCREENSHOT_E2E_512_BMP}
        -DEXPECT_MAGIC=424d
        -DEXPECT_SIZE=1000
        -DEXPECT_WIDTH=512
        -DEXPECT_HEIGHT=256
        -P ${SCREENSHOT_TEST_SCRIPT}
)

# Golden tests: run v6emul with test ROM and compare stdout to expected output
set(GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/golden")

add_test(
    NAME golden_test_port
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--rom;${GOLDEN_DIR}/test_port.bin;--load-addr;0;--halt-exit"
        -DEXPECTED=${GOLDEN_DIR}/test_port.expected
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.cmake
)

add_test(
    NAME golden_test_arith
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--rom;${GOLDEN_DIR}/test_arith.bin;--load-addr;0;--halt-exit"
        -DEXPECTED=${GOLDEN_DIR}/test_arith.expected
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.cmake
)

add_test(
    NAME golden_test_boot_rom
    COMMAND ${CMAKE_COMMAND}
        -DCMD=$<TARGET_FILE:v6emul>
        "-DARGS=--boot-rom;${CMAKE_SOURCE_DIR}/res/boot/boot.bin;--run-cycles;1;--dump-cpu"
        -DEXPECTED=${GOLDEN_DIR}/test_boot_rom.expected
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.cmake
)

# ASM unit tests (assemble + run + validate via Python).
# Prefer the project-local virtual environment (created with `uv venv .venv`
# or `python -m venv .venv`) so no system-wide Python installation is needed.
if(WIN32)
    set(_V6EMUL_VENV_PYTHON "${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe")
else()
    set(_V6EMUL_VENV_PYTHON "${CMAKE_SOURCE_DIR}/.venv/bin/python3")
    if(NOT EXISTS "${_V6EMUL_VENV_PYTHON}")
        set(_V6EMUL_VENV_PYTHON "${CMAKE_SOURCE_DIR}/.venv/bin/python")
    endif()
endif()
if(EXISTS "${_V6EMUL_VENV_PYTHON}")
    set(Python3_EXECUTABLE "${_V6EMUL_VENV_PYTHON}" CACHE FILEPATH "Python interpreter from the project .venv" FORCE)
    message(STATUS "Using project virtual environment Python: ${Python3_EXECUTABLE}")
elseif(DEFINED Python3_EXECUTABLE AND NOT EXISTS "${Python3_EXECUTABLE}")
    # Drop a stale cached path (for example a previously used .venv that was removed).
    unset(Python3_EXECUTABLE CACHE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(WIN32)
    set(V6ASM_NAMES v6asm.exe v6asm)
else()
    set(V6ASM_NAMES v6asm)
endif()

find_program(
    V6ASM_EXECUTABLE
    NAMES ${V6ASM_NAMES}
    PATHS ${CMAKE_SOURCE_DIR}/tests/tools/v6asm
    NO_DEFAULT_PATH
)

if(V6ASM_EXECUTABLE)
    add_test(
        NAME asm_unit_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_unit_tests.py
            --asm ${V6ASM_EXECUTABLE}
            --emu $<TARGET_FILE:v6emul>
            --test-dir ${CMAKE_CURRENT_SOURCE_DIR}/unit_tests
            --out-dir ${CMAKE_BINARY_DIR}/tests/unit_tests_out
            --expected ${CMAKE_CURRENT_SOURCE_DIR}/unit_tests/expected.json
    )
else()
    message(STATUS "asm_unit_tests disabled: no native v6asm executable found")
endif()
