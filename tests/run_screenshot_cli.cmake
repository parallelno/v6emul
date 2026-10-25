# Verifies screenshot-related CLI behavior.
# Usage: cmake -DCMD=<exe> "-DARGS=<arg1;arg2;...>" -DEXPECT_EXIT=<code>
#              [-DEXPECT_STDOUT=<text>] [-DEXPECT_STDERR=<text>]
#              [-DEXPECT_MAGIC=<hex>] [-DEXPECT_SIZE=<bytes>] -P run_screenshot_cli.cmake

execute_process(
    COMMAND ${CMD} ${ARGS}
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE  actual_stderr
    RESULT_VARIABLE exit_code
)

if(NOT exit_code EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
        "Unexpected exit code: expected ${EXPECT_EXIT}, got ${exit_code}\n"
        "stdout: ${actual_stdout}\nstderr: ${actual_stderr}")
endif()

if(DEFINED EXPECT_STDOUT)
    string(FIND "${actual_stdout}" "${EXPECT_STDOUT}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR
            "stdout does not contain '${EXPECT_STDOUT}'\nstdout: ${actual_stdout}")
    endif()
endif()

if(DEFINED EXPECT_STDOUT2)
    string(FIND "${actual_stdout}" "${EXPECT_STDOUT2}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR
            "stdout does not contain '${EXPECT_STDOUT2}'\nstdout: ${actual_stdout}")
    endif()
endif()

if(DEFINED EXPECT_STDERR)
    string(FIND "${actual_stderr}" "${EXPECT_STDERR}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR
            "stderr does not contain '${EXPECT_STDERR}'\nstderr: ${actual_stderr}")
    endif()
endif()

if(DEFINED EXPECT_FILE)
    if(NOT EXISTS "${EXPECT_FILE}")
        message(FATAL_ERROR "Expected screenshot file was not created: ${EXPECT_FILE}")
    endif()

    if(DEFINED EXPECT_MAGIC)
        file(READ "${EXPECT_FILE}" magic HEX LIMIT 2)
        string(TOLOWER "${magic}" magic)
        string(TOLOWER "${EXPECT_MAGIC}" expected_magic)
        if(NOT magic STREQUAL expected_magic)
            message(FATAL_ERROR
                "Unexpected file magic: expected ${EXPECT_MAGIC}, got ${magic}")
        endif()
    endif()

    if(DEFINED EXPECT_SIZE)
        file(SIZE "${EXPECT_FILE}" actual_size)
        if(actual_size LESS EXPECT_SIZE)
            message(FATAL_ERROR
                "Screenshot is too small: expected at least ${EXPECT_SIZE}, got ${actual_size}")
        endif()
    endif()

    if(DEFINED EXPECT_WIDTH OR DEFINED EXPECT_HEIGHT)
        file(READ "${EXPECT_FILE}" width_hex HEX OFFSET 18 LIMIT 4)
        file(READ "${EXPECT_FILE}" height_hex HEX OFFSET 22 LIMIT 4)

        set(width 0)
        set(height 0)
        foreach(i RANGE 3)
            math(EXPR offset "${i} * 2")
            math(EXPR shift "${i} * 8")
            string(SUBSTRING "${width_hex}" ${offset} 2 width_byte_hex)
            string(SUBSTRING "${height_hex}" ${offset} 2 height_byte_hex)
            math(EXPR width_byte "0x${width_byte_hex}")
            math(EXPR height_byte "0x${height_byte_hex}")
            math(EXPR width "${width} + (${width_byte} << ${shift})")
            math(EXPR height "${height} + (${height_byte} << ${shift})")
        endforeach()

        if(DEFINED EXPECT_WIDTH AND NOT width EQUAL EXPECT_WIDTH)
            message(FATAL_ERROR
                "Unexpected BMP width: expected ${EXPECT_WIDTH}, got ${width}")
        endif()
        if(DEFINED EXPECT_HEIGHT AND NOT height EQUAL EXPECT_HEIGHT)
            message(FATAL_ERROR
                "Unexpected BMP height: expected ${EXPECT_HEIGHT}, got ${height}")
        endif()
    endif()
endif()

message(STATUS "Screenshot CLI test PASSED")