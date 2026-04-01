# Vector-06C Emulator — Design Spec

### References

| Project | URL |
|---------|-----|
| **Devector** | <https://github.com/parallelno/Devector> |
| **v6_assembler** | <https://github.com/parallelno/v6_assembler> |

---

## 1. Product

A **C++ CLI executable** (`v6emul`) that emulates the Vector-06C (Soviet PC with Intel 8080 / KR580VM80A CPU). It serves as:

- A **backend** for external frontends (VS Code extension, standalone ImGui app) communicating over IPC.
- A **test harness** for [v6_assembler](https://github.com/parallelno/v6_assembler) — run assembled ROMs headlessly and capture test output.

---

## 2. CLI Behavior

```
v6emul [OPTIONS] [ROM_FILE]
```

- **ROM_FILE** (optional positional): path to a ROM file to load into emulator memory at `--load-addr` (default `0x0000`).
- **No ROM**: start with empty memory, accept external control commands over IPC.
- **No stop condition** (`--halt-exit`, `--run-frames`, `--run-cycles`): start TCP IPC server and run indefinitely.
- **With stop condition**: run headlessly, print test output to stdout, exit.

### Stop conditions

| Flag | Behavior |
|------|----------|
| `--halt-exit` | Exit on first HLT instruction |
| `--run-frames <N>` | Run for N frames then exit |
| `--run-cycles <N>` | Run for N CPU cycles then exit |

### Dump flags (print on exit)

| Flag | Output |
|------|--------|
| `--dump-cpu` | Full CPU state (registers, flags, PC, SP, cycles) |
| `--dump-memory` | Full 64K memory dump (hex) |
| `--dump-ramdisk <N>` | RAM-disk N (0–7) contents (hex) |

### Other options

| Flag | Purpose |
|------|---------|
| `--load-addr <ADDR>` | ROM load address in hex (default: `0x0000`) |
| `--tcp-port <PORT>` | TCP port for IPC server (default: `9876`) |
| `--speed <SPEED>` | Execution speed: `1%`, `20%`, `50%`, `100%`, `200%`, `max` |
| `--log-level <LEVEL>` | Log verbosity: `error`, `warn`, `info`, `debug`, `trace` |

---

## 3. IPC

The emulator communicates with external frontends over **TCP loopback** on `localhost`.

### Transport

- **TCP only.** Single transport for all data (commands, responses, frames). No shared memory, no dual-transport.
- Throughput: 768 × 312 × 3 bytes (RGB24) × 50.05 fps ≈ 36 MB/s. TCP loopback delivers ~700 MB/s — 20× headroom.
- Cross-platform: same socket API on Windows/Linux/macOS.

### Wire format

- **MessagePack**, serialized via `nlohmann::json::to_msgpack()` / `from_msgpack()` (built-in, no extra library).
- Length-prefixed framing: `uint32_t` length + MessagePack payload.
- Frame pixel data uses `json::binary_t` → raw bytes in MessagePack (no base64).

### Protocol

- **Pure request/response.** Client sends a request, emulator replies with a response. One response per request.
- The wire protocol mirrors `Hardware::Request(Req, json)` directly — the `Req` enum from `hardware_consts.h` (~96 command types) is used as-is on the wire.
- Frames are **polled** by the client via `GET_DISPLAY_DATA` at its own render rate.

```jsonc
// Client → Emulator:
{ "cmd": 1, "data": { ... } }        // cmd = Req enum value

// Emulator → Client:
{ "ok": true, "data": { ... } }      // result from Hardware::Request()
```

---

## 4. Emulator as a Test Tool for v6_assembler

The emulator always captures `OUT` instructions to the test port (`0xED`, see [io.cpp#L287](https://github.com/parallelno/Devector/blob/331dd83c/src/core/io.cpp#L287)) and prints them to stdout:

```
TEST_OUT port=0xED value=0x42
TEST_OUT port=0xED value=0x00
HALT at PC=0x0105 after 847231 cpu_cycles 1200 frames
```

The exit line (printed when a stop condition triggers) reports both `cpu_cycles` and `frames`. This format is consumed by test runners for deterministic assertion.

---

## 5. Codebase Origin: Fork from Devector

The emulator core is forked from [Devector](https://github.com/parallelno/Devector):

- **`core/`** → `libs/v6core/` — full emulation engine + debug subsystem (CPU, memory, I/O, display, timers, sound, FDC, debugger, breakpoints, watchpoints, disassembler, recorder, Lua scripts).
- **`utils/`** → `libs/v6utils/` — types, threading primitives (`TQueue<T>`), JSON helpers, file I/O, string utils, CLI argument parsing (`args_parser`).

The WPF build (`HAL.vcxproj`) proves these compile as a standalone library with **zero ImGui/SDL windowing/OpenGL dependencies**.

### Adaptations (only 2 files)

| File | Change |
|------|--------|
| `audio.h/cpp` | Remove SDL_AudioStream → raw sample ring buffer |
| `keyboard.h/cpp` | Remove SDL scancodes → abstract `SetKey(row, col, pressed)` |

### Not forked

| File | Reason |
|------|--------|
| `gl_utils.h/cpp` | OpenGL rendering — not needed |
| `halwrapper.h/cpp` | C++/CLI bridge — replaced by TCP IPC |
| `win_gl_utils.h/cpp` | Win32 OpenGL — not needed |
| `main_imgui/` | ImGui frontend — not needed |

### Key existing interfaces (used as-is)

- `Hardware::Request(Req, json) → Result<json>` — command-queue API with ~96 request types.
- `TQueue<T>` — thread-safe bounded queue (mutex + condition_variable).
- Debugger attached opt-in via `DEBUG_ATTACH` callback.

---

## 6. Architecture

### Thread model

Two threads:

- **Emulation thread** (spawned): runs `Hardware::Execution()`, owns all mutable core state. Single-threaded, no shared mutexes on hot path. Processes requests between frames via `ReqHandling()`.
- **Main thread**: runs TCP server loop (accept → recv → `Hardware::Request()` → send). Bridges v6ipc and v6core. In headless test mode, drives emulation directly without TCP.

### Library hierarchy

| Library | Role |
|---------|------|
| **v6utils** | Independent. Types, threading, JSON helpers, file I/O, arg parsing. |
| **v6core** | Depends on v6utils. Full emulation engine + debug. No IPC knowledge. |
| **v6ipc** | Depends on v6utils. TCP transport + MessagePack protocol. No emulation knowledge. |
| **app** | Links all three. Wires IPC to core. CLI entry point. |

### Dependencies

| Library | Source | Purpose |
|---------|--------|---------|
| `nlohmann::json` | FetchContent | JSON + MessagePack serialization |
| `LuaJIT` | ExternalProject_Add | Lua scripting (debug subsystem) |

Platform APIs: TCP sockets (POSIX / Winsock2), `std::thread` + `std::atomic` (C++20).

### Build

- CMake 3.21+ with presets (Debug/Release/CI).
- C++20 standard.
- Static libraries by default.
- CTest with standalone test executables (no test framework).
