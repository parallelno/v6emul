# Vector-06C Emulator — Implementation Plan

### References

| Project | URL | Role |
|---------|-----|------|
| **Devector** | <https://github.com/parallelno/Devector> | Existing C++ emulator — source of truth for extraction |
| Devector core sources | <https://github.com/parallelno/Devector/tree/master/src/core> | C++ files to extract into `v6core` library |
| Display spec (`display.h`) | <https://github.com/parallelno/Devector/blob/master/src/core/display.h> | Frame format, timing constants, scanline layout |
| Test-output port (`io.cpp`) | <https://github.com/parallelno/Devector/blob/331dd83c/src/core/io.cpp#L287> | Port `0xED` definition for test harness output |
| **v6_assembler** | <https://github.com/parallelno/v6_assembler> | Assembler whose ROMs this emulator validates |

---

## 1. C++ Project Layout

```
v6emul/
├── CMakeLists.txt              # top-level CMake (workspace root)
├── CMakePresets.json            # build presets (Debug, Release, CI)
├── design.md                   # design spec (existing)
├── plan.md                     # this document
│
├── libs/
│   ├── v6core/                 # emulator core static library
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── v6core/
│   │   │       ├── cpu.h           # Intel 8080 CPU emulation
│   │   │       ├── memory.h        # 64K RAM + 8×256K RAM Disks
│   │   │       ├── memory_consts.h # memory layout constants
│   │   │       ├── io.h            # I/O port subsystem (8255 PPI, timers, palette)
│   │   │       ├── display.h       # scanline rasterizer (768×312 framebuffer)
│   │   │       ├── timer_i8253.h   # i8253 programmable interval timer (3 counters)
│   │   │       ├── sound_ay8910.h  # AY-3-8910 sound chip
│   │   │       ├── fdc1793.h       # WD1793 floppy disk controller
│   │   │       ├── fdd_consts.h    # floppy-disk geometry constants
│   │   │       ├── keyboard.h      # keyboard matrix scanning
│   │   │       ├── audio.h         # audio mixing / downsampling (1.5MHz → 50kHz)
│   │   │       ├── hardware.h      # orchestrator: emulation loop, timing, debug interface
│   │   │       ├── hardware_consts.h # timing constants (clocks, frame geometry)
│   │   │       ├── breakpoint.h    # single breakpoint definition (addr, conditions, page filter)
│   │   │       ├── breakpoints.h   # breakpoint collection manager
│   │   │       ├── watchpoint.h    # single memory watchpoint definition
│   │   │       ├── watchpoints.h   # watchpoint collection manager
│   │   │       └── types.h         # shared type aliases (Addr, GlobalAddr, etc.)
│   │   └── src/
│   │       ├── cpu.cpp
│   │       ├── memory.cpp
│   │       ├── io.cpp
│   │       ├── display.cpp
│   │       ├── timer_i8253.cpp
│   │       ├── sound_ay8910.cpp
│   │       ├── fdc1793.cpp
│   │       ├── keyboard.cpp
│   │       ├── audio.cpp
│   │       ├── hardware.cpp
│   │       ├── breakpoint.cpp
│   │       ├── breakpoints.cpp
│   │       ├── watchpoint.cpp
│   │       └── watchpoints.cpp
│   │
│   └── v6ipc/                  # IPC protocol and transport static library
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── v6ipc/
│       │       ├── protocol.h      # message types, serialization (MessagePack)
│       │       ├── transport.h     # shared-memory ring buffer + named-pipe signaling
│       │       └── commands.h      # typed command/response enums
│       └── src/
│           ├── protocol.cpp
│           ├── transport.cpp
│           └── commands.cpp
│
├── app/                        # CLI binary
│   ├── CMakeLists.txt
│   └── main.cpp                # argument parsing, ROM loading, IPC server, test-output
│
└── tests/
    ├── CMakeLists.txt
    ├── cpu_tests.cpp               # instruction-level unit tests
    ├── memory_tests.cpp            # memory mapping unit tests
    ├── determinism_tests.cpp       # replay / determinism verification
    ├── integration_tests.cpp       # full ROM execution tests
    ├── ipc_tests.cpp               # IPC round-trip tests
    └── golden/                     # golden-file test fixtures (ROM + expected stdout)
```

### Rationale

| Library | Role | Why separate? |
|---------|------|---------------|
| **v6core** | Pure emulator engine | Reusable by any frontend (ImGui, VS Code, tests) without pulling in IPC or CLI deps. No networking code. |
| **v6ipc** | Transport + protocol | Isolates serialization format and transport choice; frontends only depend on this + v6core. |
| **app** (v6emul) | Binary entry point | Thin wrapper: parses args, wires IPC transport to v6core, handles test-output mode. |

### Build System

- **CMake 3.21+** with presets for Debug/Release/CI.
- C++20 standard (for `std::format`, `std::bit_width`, designated initializers).
- Libraries built as `STATIC` by default; `SHARED` option for embedding use cases.
- Testing via **Google Test** (fetched via `FetchContent`).
- Single `cmake --build` produces `v6emul` binary + test runner.

---

## 2. Runtime Model

### 2.1 Emulation Loop

The inner loop mirrors the existing [`Hardware::ExecuteInstruction()`](https://github.com/parallelno/Devector/tree/master/src/core) cycle-accurate model:

```cpp
while (running) {
    // Per machine cycle (4 T-states @ 3 MHz = 1.333 µs):
    display.Rasterize();             // 4 pixels @ 12 MHz
    cpu.ExecuteMachineCycle(irq);    // advance 1 machine cycle
    audio.Clock(2);                  // 2 ticks @ 1.5 MHz

    io.TryCommit(colorIdx);          // deferred port commits

    if (cpu.IsInstructionExecuted()) {
        CheckBreakpoints();
        CheckHltStop();              // --halt-exit mode
        HandleTestPortOutput();      // port 0xED capture
    }

    if (display.IsFrameComplete()) {
        PublishFrame();              // push to IPC ring buffer
        HandlePendingCommands();     // process pause/step/reset
        ThrottleTo50Fps();           // sleep if real-time mode
    }
}
```

### 2.2 Timing Constants (from [Devector C++ source](https://github.com/parallelno/Devector/tree/master/src/core))

| Parameter | Value | Source |
|-----------|-------|--------|
| CPU clock | 3 MHz | [`hardware_consts.h`](https://github.com/parallelno/Devector/blob/master/src/core/hardware_consts.h) |
| Display clock | 12 MHz (4× CPU) | [`display.h`](https://github.com/parallelno/Devector/blob/master/src/core/display.h) |
| Audio clock | 1.5 MHz (÷2 CPU) | [`audio.h`](https://github.com/parallelno/Devector/blob/master/src/core/audio.h) |
| Cycles per frame | 59,904 (312 lines × 192 cycles) | [`display.h`](https://github.com/parallelno/Devector/blob/master/src/core/display.h) |
| Frame rate | 50.05 FPS (3 MHz ÷ 59,904) | [`display.h`](https://github.com/parallelno/Devector/blob/master/src/core/display.h) |
| VSYNC period | 19,968 µs | [`display.h`](https://github.com/parallelno/Devector/blob/master/src/core/display.h) |
| Scanline | 192 CPU cycles = 768 pixels | [`display.h`](https://github.com/parallelno/Devector/blob/master/src/core/display.h) |

### 2.3 Execution Modes

| Mode | Behavior |
|------|----------|
| **Run** | Continuous emulation at selected speed (1%–MAX). |
| **Pause** | Halted; responds to IPC commands. |
| **Step** | Execute one instruction, then pause. |
| **Step Over** | Execute until PC passes current instruction (skip calls). |
| **Frame Step** | Execute one full frame, then pause. |
| **Reset** | Reinitialize CPU + peripherals, keep loaded ROM. |
| **Halt-Exit** | CLI flag `--halt-exit`: terminate process on first HLT instruction. |

### 2.4 Thread Model

```
┌───────────────────────────────────┐
│         Emulation Thread          │ ← owns all mutable core state
│  (Hardware::Run())                │    single-threaded, no shared mutexes on hot path
│  loops: cpu + display + audio     │
│  checks command queue each frame  │
└──────────┬────────────────────────┘
           │ lock-free queue (commands↓, events↑)
           │ shared-memory ring buffer (frames↑)
┌──────────┴────────────────────────┐
│          IPC Thread               │ ← v6ipc transport
│  reads external requests          │
│  writes responses + frames        │
└───────────────────────────────────┘
```

The emulation thread is the **sole owner** of core state — no shared locks on the hot path. Communication with the IPC thread uses a lock-free SPSC queue (e.g., `moodycamel::ReaderWriterQueue`) for commands/events and a shared-memory ring buffer for frames.

---

## 3. IPC Approach

### 3.1 Options Considered

| Option | Throughput | Latency | Cross-platform | Complexity |
|--------|-----------|---------|----------------|------------|
| TCP sockets | Good (~700 MB/s loopback) | ~50 µs | ✅ | Low |
| Unix domain sockets | Good (~1 GB/s) | ~10 µs | Linux/macOS only | Low |
| Named pipes | Moderate (~500 MB/s) | ~20 µs | ✅ (Win32 + Unix FIFO) | Medium |
| **Shared memory + signaling** | **Best (~5 GB/s)** | **~1 µs** | ✅ (platform shims) | Medium |
| gRPC / HTTP | Good | ~100 µs | ✅ | High |

### 3.2 Recommendation: Shared Memory Ring Buffer + Named Pipe Signaling

**Primary transport: shared memory** for frame streaming.
**Signaling / control: named pipe** (or Unix domain socket) for commands and low-frequency data.

#### Why?

- **Frame math**: 768 × 312 × 3 bytes (RGB24) = **719,424 bytes/frame** × 50.05 fps ≈ **~36 MB/s**.
  TCP loopback can handle this, but shared memory eliminates the kernel copy entirely — critical for sustaining 50 Hz without jitter. The frame is written once into the ring buffer; the consumer reads it directly.

- **Cross-platform**: Shared memory via `mmap` (POSIX) / `CreateFileMapping` (Win32). Signaling via named pipe (Windows) or Unix domain socket (POSIX), abstracted behind a platform shim.

- **Separation of concerns**: Large frame blits go through zero-copy shmem; small control messages (pause/step/registers) go through the named pipe with MessagePack encoding. This avoids head-of-line blocking.

#### Transport Layout

```
Shared Memory Region (e.g., 4 MB):
┌──────────────────────────────────────────┐
│ Header (64 bytes, cache-line aligned)    │
│   write_idx: std::atomic<uint64_t>       │
│   read_idx:  std::atomic<uint64_t>       │
│   frame_size: uint32_t                   │
│   slot_count: uint32_t (e.g., 4 slots)  │
├──────────────────────────────────────────┤
│ Slot 0: uint8_t[719'424]  (frame 0)     │
│ Slot 1: uint8_t[719'424]  (frame 1)     │
│ Slot 2: uint8_t[719'424]  (frame 2)     │
│ Slot 3: uint8_t[719'424]  (frame 3)     │
└──────────────────────────────────────────┘

Named Pipe / UDS (bidirectional):
  → Client sends: commands (MessagePack)
  ← Server sends: responses, register dumps, breakpoint data (MessagePack)
  ← Server sends: frame-ready notifications (4-byte sequence number)
```

### 3.3 Fallback

If shared memory is unavailable (e.g., sandboxed VS Code remote), fall back to **TCP loopback** with the same MessagePack protocol. Frames are sent inline as binary blobs with a length prefix. This adds one `memcpy` but still sustains 50 Hz on modern machines.

---

## 4. Message Protocol

### 4.1 Encoding: MessagePack

- Compact binary format (~30% smaller than JSON).
- Zero-copy deserialization via `msgpack-c` (`msgpack::unpacked`).
- Mature C++ library with header-only option.
- Easily consumed from TypeScript (`@msgpack/msgpack`) and other languages.

### 4.2 Message Shape

Every message is a length-prefixed MessagePack blob:

```
[ uint32_t length ][ MessagePack payload ]
```

### 4.3 Command Messages (Client → Emulator)

```cpp
enum class CommandType : uint8_t {
    // Execution control
    Run, Pause, Step, StepOver, StepFrame, Reset, SetSpeed,

    // Memory / ROM
    LoadRom, ReadMemory, WriteMemory,

    // Registers
    GetRegisters, SetRegisters,

    // Breakpoints
    AddBreakpoint, RemoveBreakpoint, ListBreakpoints, EnableBreakpoint,

    // Display
    RequestFrame, SubscribeFrames,

    // I/O
    GetPortState,

    // Script / eval
    RunScript,

    // Lifecycle
    Ping, Shutdown,
};

struct Command {
    CommandType type;
    // Payload varies by type; deserialized from MessagePack map.
    // Examples:
    //   LoadRom:        { "addr": uint16_t, "data": bin }
    //   ReadMemory:     { "addr": uint16_t, "len": uint16_t }
    //   SetSpeed:       { "speed": uint8_t }  // percentage or 0xFF=max
    //   AddBreakpoint:  { "addr": uint16_t, "condition": uint8_t, "page": int8_t }
    //   SubscribeFrames:{ "enabled": bool }
    MSGPACK_DEFINE(type, /* payload fields */);
};
```

### 4.4 Event Messages (Emulator → Client)

```cpp
enum class EventType : uint8_t {
    // Execution state
    StateChanged, BreakpointHit, StepComplete,

    // Data responses
    Registers, MemoryData, PortState, BreakpointList,

    // Frame
    Frame,          // via named pipe fallback; normally via shmem
    FrameReady,     // notification; data is in shmem

    // Test output
    TestOutput,

    // Script result
    ScriptResult,

    // Lifecycle
    Pong, Error,
};

struct Event {
    EventType type;
    // Payload varies by type; serialized to MessagePack map.
    // Examples:
    //   Registers:      { "pc": u16, "sp": u16, "a": u8, ... "cc": u64 }
    //   FrameReady:     { "seq": uint64_t }
    //   BreakpointHit:  { "id": u32, "addr": u16 }
    //   Error:          { "code": u32, "message": string }
    MSGPACK_DEFINE(type, /* payload fields */);
};
```

### 4.5 Frame Streaming Protocol (via Shared Memory)

1. Emulator writes RGB24 frame into `shmem[write_idx % slot_count]`.
2. Emulator increments `write_idx` (atomic release).
3. Emulator sends `FrameReady { seq }` over the named pipe.
4. Client reads `shmem[seq % slot_count]` (atomic acquire on `write_idx`).
5. Client advances `read_idx` when done.

If the client falls behind, the emulator can skip frames (overwrite oldest unread slot). The ring buffer depth (4 slots) provides tolerance for brief stalls.

---

## 5. Testing Plan

### 5.1 Unit Tests — CPU Instructions

**Framework**: Google Test (via CMake `FetchContent`).
**Location**: `tests/cpu_tests.cpp`

| Category | Approach |
|----------|----------|
| **All 256 opcodes** | Table-driven tests: for each opcode, set up initial registers/memory, execute, assert final state (registers, flags, memory, cycle count). |
| **Flag behavior** | Dedicated tests for carry, zero, sign, parity, aux-carry across arithmetic/logic ops. |
| **Interrupt handling** | Test EI/DI/RST sequences, verify interrupt vector dispatch. |
| **HLT** | Verify CPU halts and can be resumed by interrupt. |
| **Undocumented behavior** | Preserve known quirks from Devector (e.g., DAA edge cases). |

**Data source**: Use the well-known i8080 test suite (8080EX1.COM / 8080PRE.COM) as integration ROM tests, and hand-craft unit tests for individual instructions.

### 5.2 Peripheral Unit Tests

| Component | Key tests |
|-----------|-----------|
| **Memory** | Read/write main RAM; RAM Disk page switching; stack-mode mapping; address translation correctness. |
| **I/O** | Port IN/OUT dispatch; commit timer delays (1/5/8 pixel); palette writes; display mode switching. |
| **Display** | Scanline rasterization produces correct pixel indices; border colors; scroll offset; MODE_256 vs MODE_512 pixel duplication. |
| **Timer i8253** | Counter modes (0–5); read-back; underflow interrupt generation. |
| **AY-3-8910** | Register read/write; tone period → frequency; envelope shapes. |
| **FDC1793** | Command parsing; sector read/write with mock disk image; status register bits. |
| **Breakpoints** | Add/remove/enable/disable; collection lookup by address; page filtering. |
| **Watchpoints** | Add/remove/enable/disable; memory-range hit detection; read/write mode filtering. |

### 5.3 Determinism Tests

Run the same ROM with identical initial state twice; assert byte-identical frame output and cycle count. This catches any use of random state or uninitialized memory.

### 5.4 Integration Tests (ROM Execution)

- Load known-good ROM binaries (e.g., 8080EX1.COM, small demo programs).
- Run for N frames or until HLT.
- Assert final register/memory state or captured frame hash.

### 5.5 Golden Tests (Test-Port Output)

**Purpose**: Validate the [`v6asm`](https://github.com/parallelno/v6_assembler) test harness flow.

1. Assemble a small test ROM that performs `OUT 0xED, <value>` for each test assertion.
2. Run the emulator in `--halt-exit` mode.
3. Capture stdout.
4. Compare against a golden file (`tests/golden/<test_name>.expected`).

**Stdout format** (structured, machine-parseable):

```
TEST_OUT port=0xED value=0x01
TEST_OUT port=0xED value=0x00
...
HALT at PC=0x1234 after 12345 cpu_cycles
```

### 5.6 IPC Tests

- Round-trip: send `Ping` command, assert `Pong` event.
- Frame subscription: subscribe, run 3 frames, assert 3 `FrameReady` notifications.
- Breakpoint: set breakpoint, run ROM, assert `BreakpointHit`.
- Memory read/write round-trip.

---

## 6. Performance Considerations

### 6.1 Zero-Copy Frame Path

- The display rasterizer writes directly into a pre-allocated `std::array<uint8_t, FRAME_LEN * 3>` (RGB24).
- On frame completion, triple-buffer swap: `front ↔ back ↔ render`. No allocation.
- For IPC: the render buffer is `memcpy`'d into the shared-memory slot (one copy, ~0.7 ms for 700 KB at memory bandwidth). Alternative: rasterize directly into shmem if the consumer is fast enough.

### 6.2 Avoiding Allocations in the Hot Loop

- All per-instruction/per-cycle work uses stack-allocated or pre-allocated buffers.
- No `std::vector` growth, no `new`, no `std::string` formatting on the hot path.
- Breakpoint checking uses a pre-sorted `std::vector<Breakpoint>` with `std::lower_bound` on PC, not a `std::unordered_map`.

### 6.3 CPU Dispatch

- The existing Devector design uses a large `switch` statement in `Decode()` for 256-opcode dispatch. This is retained as-is — modern compilers (GCC/Clang/MSVC) generate efficient jump tables from dense switch statements.
- Each instruction handler is cycle-accurate with per-machine-cycle `switch(MC)` decomposition, matching the existing Devector architecture.

### 6.4 Display Rasterizer

- Pre-bake the full 256-entry `vector_color → RGB` palette at init time (as in Devector).
- The existing rasterizer design is retained; the inner pixel loop operates directly on the frame buffer array with computed offsets.
- Batch 16 pixels (4 CPU cycles) per rasterize call to amortize function-call overhead.

### 6.5 Audio Buffering

- Ring buffer of 4000 samples (~80 ms at 50 kHz).
- Downsampling from 1.5 MHz: accumulate 30 samples, average. No heap allocation.

### 6.6 IPC Throughput

- Shared memory ring buffer: 4 slots × 719,424 bytes ≈ 2.8 MB. Fits in L3 cache on most CPUs.
- Frame notification is a 12-byte message over named pipe (negligible latency).
- Control commands are small (<1 KB) and infrequent; MessagePack overhead is negligible.

---

## 7. Extraction Strategy from [Devector](https://github.com/parallelno/Devector)

### 7.1 Component Mapping

All C++ source files below are from [`Devector/src/core/`](https://github.com/parallelno/Devector/tree/master/src/core).

| Devector Source | v6core Target | Extraction Notes |
|-----------------|---------------|------------------|
| `cpu_i8080.h/cpp` | `cpu.h/cpp` | Direct copy. Remove `#include "utils/utils.h"` dependency; inline needed helpers. |
| `memory.h/cpp` + `memory_consts.h` | `memory.h/cpp` + `memory_consts.h` | Remove file I/O (ROM/RamDisk persistence) — move to CLI layer. Remove `utils/utils.h` dependency. |
| `io.h/cpp` | `io.h/cpp` | Remove GUI-related debug state if present. Keep commit timer logic intact. |
| `display.h/cpp` | `display.h/cpp` | Remove ImGui/OpenGL rendering. Keep scanline rasterizer, output raw RGB24 buffer. |
| `timer_i8253.h/cpp` | `timer_i8253.h/cpp` | Direct copy. |
| `sound_ay8910.h/cpp` | `sound_ay8910.h/cpp` | Direct copy. |
| `fdc_wd1793.h/cpp` + `fdd_consts.h` | `fdc1793.h/cpp` + `fdd_consts.h` | Direct copy. Abstract disk image source (file path → `std::vector<uint8_t>` buffer). Keep `fdd_consts.h` for disk geometry constants. |
| `keyboard.h/cpp` | `keyboard.h/cpp` | Remove SDL keycodes. Accept abstract key events (row/column matrix). |
| `audio.h/cpp` | `audio.h/cpp` | Remove SDL audio dependency. Output raw samples to ring buffer. |
| `hardware.h/cpp` + `hardware_consts.h` | `hardware.h/cpp` + `hardware_consts.h` | Remove `std::thread`/`std::mutex` GUI-loop coupling. Expose `Run()`/`Step()` API. Replace internal request enums with IPC-compatible `Command` enum. Keep `hardware_consts.h` for timing constants. |
| `breakpoint.h/cpp` | `breakpoint.h/cpp` | Direct copy. Single breakpoint definition and logic. |
| `breakpoints.h/cpp` | `breakpoints.h/cpp` | Direct copy. Collection manager — add/remove/enable/list breakpoints. |
| `watchpoint.h/cpp` | `watchpoint.h/cpp` | Direct copy. Single memory watchpoint definition and logic. |
| `watchpoints.h/cpp` | `watchpoints.h/cpp` | Direct copy. Collection manager — add/remove/enable/list memory watchpoints. |
| `utils/types.h` | `types.h` | Extract `Addr`, `GlobalAddr`, `ColorI` typedefs. Drop GUI-only types. |

### 7.2 Key Refactoring Steps

1. **Remove GUI coupling**: Devector's `Hardware` class uses `std::thread` + `std::mutex` to synchronize with the ImGui render loop. Replace with a clean synchronous `Run()` method that the CLI/IPC layer calls.
2. **Remove SDL dependency**: Audio output, keyboard input, and window management all go through SDL in Devector. Replace with abstract interfaces:
   - Audio → write samples to `std::array` ring buffer (consumed by IPC).
   - Keyboard → accept key matrix updates via `SetKey(row, col, pressed)`.
   - Display → expose raw `uint8_t*` frame buffer pointer.
3. **Remove file I/O from core**: Devector's `Memory` constructor loads ROM/RamDisk files. Move file I/O to the CLI layer; `Memory::Init()` accepts raw byte spans.
4. **Remove `utils/utils.h` dependency**: Contains logging, file helpers, and GUI utilities. Inline the few needed helpers (`LoadFile`, logging) or replace with `<format>` / `<fstream>`.

### 7.3 Incremental Validation Strategy

Extract and validate components **bottom-up**, starting with leaf dependencies:

```
Phase 1: Foundation (no dependencies)
  ├── memory.h/cpp     — unit tests for read/write/mapping
  ├── cpu.h/cpp        — depends on memory
  │                       validate with 8080 test suite ROMs
  └── keyboard.h/cpp   — trivial; unit tests only

Phase 2: Peripherals
  ├── timer_i8253.h/cpp — unit tests for counter modes
  ├── sound_ay8910.h/cpp — unit tests for register R/W
  ├── fdc1793.h/cpp + fdd_consts.h — unit tests with mock disk
  ├── audio.h/cpp      — unit tests for downsampling
  ├── breakpoints.h/cpp — collection manager unit tests
  └── watchpoint.h/cpp + watchpoints.h/cpp — unit tests for memory watchpoints

Phase 3: I/O Orchestration
  ├── io.h/cpp         — depends on keyboard, memory, timer, ay, fdc
  │                       integration tests: port IN/OUT sequences
  └── display.h/cpp    — depends on memory, io
                          visual regression: render test patterns, compare hashes

Phase 4: Hardware Loop
  └── hardware.h/cpp   — full integration
                          run 8080EX1.COM, verify test output
                          run demo ROMs, verify frame hashes

Phase 5: IPC + CLI
  ├── v6ipc            — protocol + transport tests
  └── app/main.cpp     — end-to-end: CLI → IPC → run ROM → capture output
```

### 7.4 Behavior Drift Mitigation

1. **Cycle-count oracle**: Run the same instruction sequence in original [Devector](https://github.com/parallelno/Devector) and extracted v6core; compare per-instruction cycle counts.
2. **Frame hash oracle**: Render the same ROM for N frames in both; compare SHA-256 of each frame.
3. **Test-port oracle**: Run the same test ROM in both; compare [`OUT 0xED`](https://github.com/parallelno/Devector/blob/331dd83c/src/core/io.cpp#L287) sequences.

These oracles can be automated as CI jobs once Phase 4 is complete.

---

## 8. Dependency Summary

| Library | Source | Purpose |
|---------|--------|---------|
| `msgpack-c` | vcpkg / FetchContent | MessagePack serialization |
| `CLI11` | FetchContent | CLI argument parsing |
| `GoogleTest` | FetchContent | Unit / integration testing |
| `Google Benchmark` | FetchContent | Performance benchmarking |
| `spdlog` | vcpkg / FetchContent | Logging |
| `moodycamel::ReaderWriterQueue` | header-only / FetchContent | Lock-free SPSC queue |

Platform APIs used directly (no wrapper library):
- **Shared memory**: `mmap`/`munmap` (POSIX), `CreateFileMapping`/`MapViewOfFile` (Win32).
- **Named pipes**: `mkfifo` / Unix domain sockets (POSIX), `CreateNamedPipe` (Win32).
- **Threads**: `std::thread` + `std::atomic` (C++20 standard library).

No external GUI, async, or networking frameworks — the emulation loop is synchronous and latency-sensitive. IPC threads use blocking I/O.

---

## 9. CLI Interface

```
v6emul [OPTIONS] [ROM_FILE]

Arguments:
  [ROM_FILE]    Path to a ROM file to load (optional)

Options:
  --halt-exit           Exit on first HLT instruction (test mode)
  --load-addr <ADDR>    ROM load address in hex (default: 0x0000)
  --ipc <MODE>          IPC mode: shmem (default), tcp, none
  --ipc-name <NAME>     Shared memory / pipe name (default: "v6emul")
  --tcp-port <PORT>     TCP port for IPC (default: 9876)
  --speed <SPEED>       Execution speed: 1%, 20%, 50%, 100%, 200%, max
  --log-level <LEVEL>   Log verbosity: error, warn, info, debug, trace
  -h, --help            Print help
  -V, --version         Print version
```

### Test-Mode Output (stdout)

When running with `--halt-exit` or any ROM that performs `OUT 0xED`:

```
TEST_OUT port=0xED value=0x42
TEST_OUT port=0xED value=0x00
HALT at PC=0x0105 after 847231 cpu_cycles
```

This is consumed by test runners (e.g., [`v6asm`](https://github.com/parallelno/v6_assembler) integration tests) for deterministic assertion.

---

## 10. Implementation Order (Milestones)

### Milestone 1 — CPU + Memory (foundation)
- [ ] Project scaffolding (`CMakeLists.txt`, directory structure, presets)
- [ ] Extract `memory.h/cpp`: decouple from file I/O + unit tests
- [ ] Extract `cpu.h/cpp`: decouple from `utils/` + unit tests
- [ ] Run 8080PRE.COM / 8080EX1.COM test ROMs to validate CPU

### Milestone 2 — Peripherals
- [ ] Extract `keyboard.h/cpp`: remove SDL + unit tests
- [ ] Extract `timer_i8253.h/cpp`: unit tests
- [ ] Extract `sound_ay8910.h/cpp`: unit tests
- [ ] Extract `fdc1793.h/cpp` + `fdd_consts.h`: unit tests (mock disk)
- [ ] Extract `audio.h/cpp`: remove SDL + unit tests
- [ ] Extract `breakpoints.h/cpp`: collection manager + unit tests
- [ ] Extract `watchpoint.h/cpp` + `watchpoints.h/cpp`: unit tests

### Milestone 3 — I/O + Display
- [ ] Extract `io.h/cpp`: port dispatch + commit timers + unit tests
- [ ] Extract `display.h/cpp`: remove ImGui/GL, output raw RGB24 + unit tests
- [ ] Integration test: render a test pattern ROM, verify frame hash

### Milestone 4 — Hardware Loop + Test Mode
- [ ] Extract `hardware.h/cpp`: decouple from GUI thread model, expose `Run()`/`Step()` API
- [ ] `--halt-exit` mode + port 0xED test output to stdout
- [ ] Golden tests: assemble test ROMs with [v6asm](https://github.com/parallelno/v6_assembler), run, compare output
- [ ] Determinism test: run same ROM twice, assert identical output

### Milestone 5 — IPC
- [ ] `v6ipc/protocol`: MessagePack message types
- [ ] `v6ipc/transport`: shared-memory ring buffer + named pipe signaling
- [ ] `v6ipc/commands`: typed request/response API
- [ ] IPC integration tests (ping, frame subscribe, breakpoint)

### Milestone 6 — CLI + Polish
- [ ] `app/main.cpp`: CLI11 argument parsing, ROM loading, IPC server wiring
- [ ] TCP fallback transport
- [ ] End-to-end test: CLI → IPC → run ROM → capture frame
- [ ] Benchmarking with Google Benchmark (instructions/sec, frames/sec)
- [ ] CI pipeline (build + test on Linux/macOS/Windows)
