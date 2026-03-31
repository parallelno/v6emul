add_library(v6core STATIC
    # Core emulation
    src/cpu_i8080.cpp
    src/memory.cpp
    src/io.cpp
    src/display.cpp
    src/timer_i8253.cpp
    src/sound_ay8910.cpp
    src/fdc_wd1793.cpp
    src/keyboard.cpp
    src/audio.cpp
    src/hardware.cpp
    # Debug subsystem
    src/debugger.cpp
    src/debug_data.cpp
    src/breakpoint.cpp
    src/breakpoints.cpp
    src/watchpoint.cpp
    src/watchpoints.cpp
    src/disasm.cpp
    src/trace_log.cpp
    src/recorder.cpp
    src/script.cpp
    src/scripts.cpp
)

target_include_directories(v6core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(v6core
    PUBLIC
        v6utils
    PRIVATE
        luajit
)

add_dependencies(v6core luajit_build)
