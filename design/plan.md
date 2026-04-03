# Vector-06C Emulator — Implementation Plan

### References

| Project | URL |
|---------|-----|
| **Devector** | <https://github.com/parallelno/Devector> |
| **v6_assembler** | <https://github.com/parallelno/v6_assembler> |

---

## 0. Key Insight: The Core Is Already Decoupled

The Devector repository contains a **WPF frontend** build (`src/main_wpf/`) that compiles the entire `core/` + `utils/` layer as a C++/CLI DLL (`HAL.vcxproj`). This proves:

1. **The core has zero ImGui dependencies.** No `#ifdef WPF` conditionals — the separation is structural.
2. **`Hardware::Request(Req, json) → Result<json>`** is already a clean command-queue interface with ~80 request types covering emulation control, state queries, and debug operations.
3. **The debugger is opt-in** — attached via `DEBUG_ATTACH` callback; the emulation loop runs without it.
4. **The threading model is production-ready** — an execution thread owns all mutable state, communicates with callers via `TQueue<T>` (mutex + condition_variable).

**Strategy**: Fork `core/` + `utils/` as-is, adapt only **2 files** that depend on SDL (`audio.h`, `keyboard.h`), and replace the C++/CLI bridge (`halwrapper.cpp`) with our IPC transport layer. There is no need for file-by-file extraction.

---

## 1. C++ Project Layout

```
v6emul/
├── CMakeLists.txt              # top-level CMake (workspace root)
├── CMakePresets.json            # build presets (Debug, Release, CI)
│
├── libs/
│   ├── v6core/                 # emulator core static library (forked from Devector)
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── v6core/
│   │   │       │
│   │   │       │  # ── Core emulation ──
│   │   │       ├── cpu_i8080.h         # Intel 8080 (KR580VM80A) CPU emulation
│   │   │       ├── memory.h            # 64K RAM + 8×256K RAM Disks
│   │   │       ├── memory_consts.h     # memory layout constants
│   │   │       ├── io.h                # I/O port subsystem (8255 PPI, timers, palette)
│   │   │       ├── display.h           # scanline rasterizer (768×312 framebuffer)
│   │   │       ├── timer_i8253.h       # i8253 programmable interval timer (3 counters)
│   │   │       ├── sound_ay8910.h      # AY-3-8910 sound chip
│   │   │       ├── fdc_wd1793.h        # WD1793 floppy disk controller
│   │   │       ├── fdd_consts.h        # floppy-disk geometry constants
│   │   │       ├── keyboard.h          # keyboard matrix scanning (SDL → abstract)
│   │   │       ├── audio.h             # audio mixing / downsampling (SDL → ring buffer)
│   │   │       ├── hardware.h          # orchestrator: emulation loop, timing, request queue
│   │   │       ├── hardware_consts.h   # Req enum (~80 command types)
│   │   │       │
│   │   │       │  # ── Debug subsystem ──
│   │   │       ├── debugger.h          # debug orchestrator (step, step-over, breakpoint dispatch)
│   │   │       ├── debug_data.h        # labels, comments, debug metadata persistence
│   │   │       ├── breakpoint.h        # single breakpoint definition
│   │   │       ├── breakpoints.h       # breakpoint collection manager
│   │   │       ├── watchpoint.h        # single memory watchpoint definition
│   │   │       ├── watchpoints.h       # watchpoint collection manager
│   │   │       ├── disasm.h            # disassembler engine
│   │   │       ├── disasm_i8080_cmds.h # i8080 mnemonic tables
│   │   │       ├── disasm_z80_cmds.h   # Z80 mnemonic tables (compat)
│   │   │       ├── code_perf.h         # per-address execution profiling
│   │   │       ├── memory_edit.h       # memory patch management
│   │   │       ├── trace_log.h         # instruction-level trace logging
│   │   │       ├── recorder.h          # state recording / playback (rewind)
│   │   │       ├── script.h            # single Lua script definition
│   │   │       ├── scripts.h           # Lua script collection manager
│   │   │       │
│   │   │       │  # ── Shared types ──
│   │   │       └── types.h             # type aliases (Addr, GlobalAddr, ColorI, etc.)
│   │   └── src/
│   │       │  # ── Core emulation ──
│   │       ├── cpu_i8080.cpp
│   │       ├── memory.cpp
│   │       ├── io.cpp
│   │       ├── display.cpp
│   │       ├── timer_i8253.cpp
│   │       ├── sound_ay8910.cpp
│   │       ├── fdc_wd1793.cpp
│   │       ├── keyboard.cpp
│   │       ├── audio.cpp
│   │       ├── hardware.cpp
│   │       │  # ── Debug subsystem ──
│   │       ├── debugger.cpp
│   │       ├── debug_data.cpp
│   │       ├── breakpoint.cpp
│   │       ├── breakpoints.cpp
│   │       ├── watchpoint.cpp
│   │       ├── watchpoints.cpp
│   │       ├── disasm.cpp
│   │       ├── trace_log.cpp
│   │       ├── recorder.cpp
│   │       ├── script.cpp
│   │       └── scripts.cpp
│   │
│   ├── v6utils/                # utility library (forked from Devector utils/)
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── v6utils/
│   │   │       ├── types.h         # Addr, GlobalAddr, Id, Idx, ColorI, Condition enum
│   │   │       ├── consts.h        # ErrCode enum, INVALID_ID, shared constants
│   │   │       ├── result.h        # Result<T> — optional<T> + ErrCode wrapper
│   │   │       ├── tqueue.h        # TQueue<T> — thread-safe bounded queue
│   │   │       ├── json_utils.h    # nlohmann::json load/save + typed getters
│   │   │       ├── str_utils.h     # string conversion helpers
│   │   │       ├── args_parser.h   # CLI argument parsing (-param value, typed getters)
│   │   │       └── utils.h         # Log(), file I/O (LoadFile/SaveFile), math helpers
│   │   └── src/
│   │       ├── json_utils.cpp
│   │       ├── str_utils.cpp
│   │       ├── args_parser.cpp
│   │       └── utils.cpp
│   │
│   └── v6ipc/                  # IPC protocol and transport static library
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── v6ipc/
│       │       ├── protocol.h      # message framing, nlohmann::json ↔ MessagePack
│       │       ├── transport.h     # TCP loopback server/client
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
| **v6core** | Full emulator engine + debug subsystem | Forked from Devector `core/`. Reusable by any frontend. No networking code. |
| **v6utils** | Shared utility layer | Forked from Devector `utils/`. Types, threading primitives, JSON helpers, file I/O. Kept separate because core + IPC both need it. |
| **v6ipc** | Transport + protocol | Isolates serialization format and transport choice; replaces the WPF C++/CLI bridge. |
| **app** (v6emul) | Binary entry point | Thin wrapper: parses args, wires IPC transport to v6core, handles test-output mode. |

### Library Dependency Hierarchy

```
v6utils          ← independent (only external dep: nlohmann::json)
   ↑  ↑
v6core  v6ipc    ← both depend on v6utils; independent of each other
   ↑      ↑
     app         ← links all three as static libraries into a single executable
```

- **v6utils** has no internal dependencies — it provides types, threading primitives, and helpers used by everything above it.
- **v6core** depends on v6utils (for `types.h`, `tqueue.h`, `json_utils.h`, etc.) but knows nothing about IPC.
- **v6ipc** depends on v6utils (for `result.h`, `tqueue.h`, serialization helpers) but knows nothing about the emulation engine.
- **app** is the only target that links all three libraries together, wiring the IPC transport to `Hardware::Request()`.

### What Comes Directly from Devector

The following is the complete file inventory from `HAL.vcxproj` — the proven library build baseline. All of these compile cleanly as a library with **no ImGui, no SDL windowing, no OpenGL rendering**:

**Core files** (from `Devector/src/core/`):

| File | SDL dep? | Lua dep? | Notes |
|------|----------|----------|-------|
| `cpu_i8080.h/cpp` | No | No | Direct copy |
| `memory.h/cpp` + `memory_consts.h` | No | No | Direct copy |
| `io.h/cpp` | No | No | Direct copy |
| `display.h/cpp` | No | No | Direct copy |
| `timer_i8253.h/cpp` | No | No | Direct copy |
| `sound_ay8910.h/cpp` | No | No | Direct copy |
| `fdc_wd1793.h/cpp` + `fdd_consts.h` | No | No | Direct copy |
| `keyboard.h/cpp` | **Yes** | No | Uses SDL scancodes — **adapt** |
| `audio.h/cpp` | **Yes** | No | Uses SDL_AudioStream — **adapt** |
| `hardware.h/cpp` + `hardware_consts.h` | No | No | Direct copy (already has clean Request API) |
| `debugger.h/cpp` | No | No | Direct copy (opt-in via DEBUG_ATTACH) |
| `debug_data.h/cpp` | No | Yes | Direct copy |
| `breakpoint.h/cpp` | No | No | Direct copy |
| `breakpoints.h/cpp` | No | No | Direct copy |
| `watchpoint.h/cpp` | No | No | Direct copy |
| `watchpoints.h/cpp` | No | No | Direct copy |
| `disasm.h/cpp` + `disasm_i8080_cmds.h` + `disasm_z80_cmds.h` | No | No | Direct copy |
| `code_perf.h` | No | No | Direct copy (header-only) |
| `memory_edit.h` | No | No | Direct copy (header-only) |
| `trace_log.h/cpp` | No | No | Direct copy |
| `recorder.h/cpp` | No | No | Direct copy |
| `script.h/cpp` | No | **Yes** | Direct copy (requires LuaJIT) |
| `scripts.h/cpp` | No | **Yes** | Direct copy (requires LuaJIT) |

**Utils files** (from `Devector/src/utils/`):

| File | Notes |
|------|-------|
| `types.h` | Fundamental types: `Addr`, `GlobalAddr`, `ColorI`, `Id`, `Idx` |
| `consts.h` | `ErrCode` enum, `INVALID_ID`, shared constants |
| `result.h` | `Result<T>` — `optional<T>` + `ErrCode` |
| `tqueue.h` | `TQueue<T>` — thread-safe bounded queue (mutex + condition_variable) |
| `json_utils.h/cpp` | nlohmann::json load/save + typed getters |
| `str_utils.h/cpp` | String conversion helpers |
| `utils.h/cpp` | `Log()`, `LoadFile()`, `SaveFile()`, math helpers |
| `args_parser.h/cpp` | CLI argument parsing (`-param value`, typed getters, auto help) |

**Not forked** (GUI-only or replaced):

| File | Reason |
|------|--------|
| `gl_utils.h/cpp` | OpenGL rendering — not needed |
| `args_parser.h/cpp` | Replaced by CLI11 |
| `halwrapper.h/cpp` + `win_gl_utils.h/cpp` | C++/CLI bridge — replaced by v6ipc |

### Build System

- **CMake 3.21+** with presets for Debug/Release/CI.
- C++20 standard (for `std::format`, `std::bit_width`, designated initializers).
- Libraries built as `STATIC` by default; `SHARED` option for embedding use cases.
- **LuaJIT**: External build via `ExternalProject_Add` (pattern from Devector's CMakeLists.txt).
- **nlohmann::json**: FetchContent (already used by Devector's utils).
- Testing via **CTest** with standalone test executables (no framework dependency).
- Single `cmake --build` produces `v6emul` binary + test executables.

---

## 2. Thread Model

Two threads:

```
┌───────────────────────────────────────────┐
│  Emulation Thread (spawned)               │ ← v6core: owns all mutable core state
│  Hardware::Execution() loop               │    single-threaded, no shared mutexes
│  cpu + display + audio per cycle          │    on hot path
│  ReqHandling() between frames             │
│  signals frame-ready via TQueue           │
└──────────┬────────────────────────────────┘
           │ TQueue (commands↓, responses↑)
┌──────────┴────────────────────────────────┐
│  Main Thread (main.cpp)                   │ ← app: wires v6ipc ↔ v6core
│  TCP accept → recv loop                  │
│  on command received:                     │
│    json::from_msgpack() → json            │
│    Hardware::Request() → response         │
│    json::to_msgpack() → send()            │
└───────────────────────────────────────────┘
```

The emulation thread is the **sole owner** of core state. `Hardware::Request()` pushes commands to a `TQueue`, blocks until the execution thread processes them, and returns the result. This pattern is already proven in the WPF build.

The main thread runs the TCP server loop: it blocks on `recv()`, deserializes incoming MessagePack via `json::from_msgpack()` into `Hardware::Request()` calls, and sends responses back. Frames are fetched on demand via `GET_DISPLAY_DATA` — the client controls its own render rate. **v6ipc and v6core are independent** — the main thread is the only place where both APIs meet (see [Library Dependency Hierarchy](#library-dependency-hierarchy)).

---

## 3. IPC Transport

**TCP loopback** on `localhost`. Single transport for commands, responses, and frame data.

#### Why TCP?

- **Throughput is sufficient**: 768 × 312 × 3 bytes (RGB24) = **719,424 bytes/frame** × 50.05 fps ≈ **~36 MB/s**. TCP loopback delivers ~700 MB/s — nearly 20× headroom.
- **Single code path**: One transport for everything (frames + commands). No platform shims, no dual-transport complexity.
- **Cross-platform for free**: Same socket API on Windows, Linux, macOS. No `mmap` vs `CreateFileMapping` shims.
- **Simple error handling**: Connection lost = socket error. No leaked shared-memory segments to clean up.
- **Latency is irrelevant**: Frames arrive every ~20 ms; control commands are human-speed. TCP's ~50 µs latency is invisible.


---

## 4. Message Protocol

### 4.1 Internal: Hardware::Request (already exists)

The existing `Hardware::Request(Req, json) → Result<json>` interface uses `nlohmann::json` as the universal data type. This is the internal command API — it already handles all ~80 request types (see `hardware_consts.h`).

### 4.2 Wire Format: nlohmann::json → MessagePack

The IPC wire format is **MessagePack**, serialized using `nlohmann::json`'s built-in support — no extra library needed.

- `nlohmann::json::to_msgpack(j)` serializes any `json` object to a compact binary `std::vector<uint8_t>`.
- `nlohmann::json::from_msgpack(bytes)` deserializes back to a `json` object.
- Frame data uses `json::binary_t` — serialized as raw bytes in MessagePack (no base64 bloat).
- Client side (TypeScript): `@msgpack/msgpack` decodes natively; binary stays as `Uint8Array`.

Since `Hardware::Request()` already returns `json`, there is **zero conversion** — the response object is serialized directly to the wire. Messages are length-prefixed on the wire (`uint32_t` length + MessagePack payload).

### 4.3 IPC ↔ Hardware Bridge (wired by app)

The main thread bridges IPC and core directly — no translation layer needed:

```cpp
// In main.cpp — recv loop:
auto msgBytes = transport.recv();                          // read length-prefixed MessagePack
auto requestJ = nlohmann::json::from_msgpack(msgBytes);    // deserialize to json
auto req = static_cast<Req>(requestJ["cmd"].get<int>());  // extract command
auto result = hardware.Request(req, requestJ["data"]);    // call v6core directly
auto responseBytes = nlohmann::json::to_msgpack(result);   // serialize response
transport.send(responseBytes);                             // send back
```

### 4.4 Messages

The client sends **requests** using the existing `Req` enum values from `hardware_consts.h` (~96 command types). No separate command enum — the wire protocol mirrors the internal API directly:

```jsonc
// Client → Emulator (request):
{ "cmd": 1, "data": { ... } }     // cmd = Req::RUN

// Emulator → Client (response):
{ "ok": true, "data": { ... } }   // result from Hardware::Request()
```

The `Req` enum covers all emulation control, state queries, debug operations, breakpoints, watchpoints, recorder, scripts, memory edits, code perf, FDC/FDD, display settings, and I/O port access. See `hardware_consts.h` for the full list.

Everything is **request/response** — including frames. The client fetches the current frame via `GET_DISPLAY_DATA` at its own render rate (e.g., `requestAnimationFrame` at ~60 fps). The response includes RGB24 data as `json::binary_t`, which serializes as raw bytes in MessagePack. No subscription, no push events, no frame-dropping logic.

---

## 5. Testing Plan

### 5.1 Unit Tests — CPU Instructions

**Framework**: Plain CTest with standalone executables (no test framework dependency).
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
| **Disassembler** | Instruction decode for all 256 opcodes; correct mnemonic + operand formatting. |

### 5.3 Determinism Tests

Run the same ROM with identical initial state twice; assert byte-identical frame output and cycle count. This catches any use of random state or uninitialized memory.

### 5.4 Integration Tests (ROM Execution)

- Load known-good ROM binaries (e.g., 8080EX1.COM, small demo programs).
- Run with `--run-frames N`, `--run-cycles N`, or `--halt-exit`.
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
- Frame fetch: run emulation, request `GET_DISPLAY_DATA`, assert valid RGB24 frame with `binary_t`.
- Breakpoint: set breakpoint, run ROM, assert `BreakpointHit`.
- Memory read/write round-trip.

---

## 6. Fork-and-Adapt Strategy from [Devector](https://github.com/parallelno/Devector)

### 6.1 Approach: Fork, Don't Extract

The Devector WPF build (`HAL.vcxproj`) proves that `core/` + `utils/` already compile as a standalone library with no ImGui dependencies. Instead of extracting files one-by-one, we **fork the entire `core/` and `utils/` directories** and make minimal targeted adaptations.

### 6.2 What Changes (only 2 files + 1 replacement)

| File | Change | Reason |
|------|--------|--------|
| **`audio.h/cpp`** | Remove `#include "SDL3/SDL.h"`, `SDL_AudioStream`, `SDL_AudioDeviceID`, `SDL_Init(SDL_INIT_AUDIO)`. Replace with raw sample ring buffer output. | Only SDL dependency in core (audio output). |
| **`keyboard.h/cpp`** | Remove `#include <SDL3/SDL.h>`, SDL scancode mapping. Replace with abstract `SetKey(row, col, pressed)` interface. | Only SDL dependency in core (key input). |
| **`halwrapper.h/cpp`** | **Not forked.** Replaced entirely by `v6ipc/` transport layer. | C++/CLI bridge is WPF-specific. |

### 6.3 What Stays As-Is (direct copy)

Everything else from `core/` and `utils/` is copied unchanged:

- **Emulation engine**: `cpu_i8080`, `memory`, `io`, `display`, `timer_i8253`, `sound_ay8910`, `fdc_wd1793`, `hardware` — all direct copies.
- **Debug subsystem**: `debugger`, `debug_data`, `breakpoint(s)`, `watchpoint(s)`, `disasm`, `trace_log`, `recorder`, `code_perf`, `memory_edit` — all direct copies. The debugger is opt-in (attached only when needed).
- **Scripting**: `script`, `scripts` with LuaJIT dependency — direct copies.
- **Utils**: `types`, `consts`, `result`, `tqueue`, `json_utils`, `str_utils`, `utils` — direct copies.
- **Constants**: `hardware_consts.h`, `memory_consts.h`, `fdd_consts.h`, `disasm_i8080_cmds.h`, `disasm_z80_cmds.h` — direct copies.

### 6.4 What's Not Forked (GUI/platform-specific)

| File | Reason |
|------|--------|
| `utils/gl_utils.h/cpp` | OpenGL rendering utilities — not needed |
| `main_wpf/HAL/halwrapper.h/cpp` | C++/CLI bridge — replaced by v6ipc |
| `main_wpf/HAL/win_gl_utils.h/cpp` | Win32 OpenGL context — not needed |
| `main_imgui/` (entire directory) | ImGui frontend — not needed |

### 6.5 Dependency Changes

| Devector dependency | v6emul status |
|---------------------|---------------|
| SDL3 | **Removed** — only used by audio.h and keyboard.h, both adapted |
| ImGui | **Not present** in core — nothing to remove |
| nlohmann::json | **Kept** — used by Hardware::Request() and debug_data |
| LuaJIT | **Kept** — used by script/scripts for Lua scripting |
| GLAD (OpenGL) | **Not forked** — only in gl_utils, which we skip |

---

## 7. Dependency Summary

| Library | Source | Purpose |
|---------|--------|---------|
| `nlohmann::json` | FetchContent | JSON data type + MessagePack wire serialization (built-in `to_msgpack`/`from_msgpack`) |
| `LuaJIT` | ExternalProject_Add | Lua scripting engine (used by script/scripts) |

Platform APIs used directly (no wrapper library):
- **TCP sockets**: `socket`/`send`/`recv` (POSIX), Winsock2 (Win32) — loopback only.
- **Threads**: `std::thread` + `std::atomic` (C++20 standard library).

---

## 8. CLI Interface

```
v6emul [OPTIONS] [ROM_FILE]

Arguments:
  [ROM_FILE]    Path to a ROM file to load (optional)

Options:
  --halt-exit           Exit on first HLT instruction (test mode)
  --run-frames <N>      Run for N frames then exit
  --run-cycles <N>      Run for N CPU cycles then exit
  --dump-cpu            Print full CPU state on exit (registers, flags, PC, SP, cycles)
  --dump-memory         Print full 64K memory dump on exit (hex)
  --dump-ramdisk <N>    Print RAM-disk N (0–7) contents on exit (hex)
  --load-addr <ADDR>    ROM load address in hex (default: 0x0000)
  --tcp-port <PORT>     TCP port for IPC server (default: 9876)
  --speed <SPEED>       Execution speed: 1%, 20%, 50%, 100%, 200%, max
  --log-level <LEVEL>   Log verbosity: error, warn, info, debug, trace
  -h, --help            Print help
  -V, --version         Print version
```

### Test-Port Output (stdout)

The emulator always prints `OUT 0xED` values to stdout, regardless of mode:

```
TEST_OUT port=0xED value=0x42
TEST_OUT port=0xED value=0x00
HALT at PC=0x0105 after 847231 cpu_cycles 1200 frames
```

The exit line always reports both `cpu_cycles` and `frames` completed, regardless of which stop condition triggered.

This is consumed by test runners (e.g., [`v6asm`](https://github.com/parallelno/v6_assembler) integration tests) for deterministic assertion.

---

## 9. Implementation Order (Milestones)

### Milestone 1 — Build + Validate Core
- [ ] CMake scaffolding: top-level `CMakeLists.txt`, presets, v6core/v6utils library targets
- [ ] Fork `core/` → `libs/v6core/`, `utils/` → `libs/v6utils/`
- [ ] Adapt `audio.h/cpp`: remove SDL, replace with raw sample ring buffer
- [ ] Adapt `keyboard.h/cpp`: remove SDL, replace with abstract key matrix interface
- [ ] Build: verify v6core + v6utils compile as static libraries
- [ ] Run 8080PRE.COM / 8080EX1.COM test ROMs to validate CPU (via test executable)
- [ ] Unit tests for memory mapping, CPU instructions

### Milestone 2 — CLI + Test Mode
- [ ] `app/main.cpp`: argument parsing (forked ArgsParser), ROM loading, `--halt-exit` / `--run-frames` / `--run-cycles`
- [ ] Port 0xED test output to stdout
- [ ] Golden tests: assemble test ROMs with [v6asm](https://github.com/parallelno/v6_assembler), run, compare output
- [ ] Determinism test: run same ROM twice, assert identical cycle count and frame output

### Milestone 3 — IPC
- [ ] `v6ipc/protocol`: message framing (length-prefixed MessagePack via nlohmann::json)
- [ ] `v6ipc/transport`: TCP loopback server (accept, recv, send)
- [ ] Wire `main.cpp`: TCP recv loop → `Hardware::Request()` → response (see §4.3)
- [ ] IPC round-trip tests (command → response, frame fetch, memory read/write)

### Milestone 4 — CI + Polish
- [ ] End-to-end test: client → TCP → run ROM → fetch frame → verify RGB24
- [ ] CI pipeline (build + test on Linux/macOS/Windows)
