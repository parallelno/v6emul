# Golden test runner: executes CMD with ARGS, compares stdout to EXPECTED file
# Usage: cmake -DCMD=<exe> -DARGS=<arg1;arg2;...> -DEXPECTED=<file> -P run_golden.cmake

# Run the command and capture output
execute_process(
    COMMAND ${CMD} ${ARGS}
    OUTPUT_VARIABLE actual_output
    ERROR_VARIABLE  stderr_output
    RESULT_VARIABLE exit_code
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR "Command failed with exit code ${exit_code}\nstderr: ${stderr_output}")
endif()

# Filter out log noise (lines starting with "Local time" or "Failed to init")
string(REPLACE "\n" ";" actual_lines "${actual_output}")
set(filtered_lines "")
foreach(line IN LISTS actual_lines)
    string(FIND "${line}" "Local time" pos_local)
    string(FIND "${line}" "Failed to init" pos_failed)
    if(pos_local EQUAL -1 AND pos_failed EQUAL -1)
        if(NOT "${line}" STREQUAL "")
            list(APPEND filtered_lines "${line}")
        endif()
    endif()
endforeach()
string(REPLACE ";" "\n" filtered_output "${filtered_lines}")

# Read expected output
file(READ "${EXPECTED}" expected_output)
string(STRIP "${expected_output}" expected_output)

# Compare
if(NOT filtered_output STREQUAL expected_output)
    message(FATAL_ERROR
        "Golden test mismatch!\n"
        "=== EXPECTED ===\n${expected_output}\n"
        "=== ACTUAL (filtered) ===\n${filtered_output}\n"
        "=== END ===")
endif()

message(STATUS "Golden test PASSED")
