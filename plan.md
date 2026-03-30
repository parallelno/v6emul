# Vector-06C Emulator — Implementation Plan

## 1. Rust Workspace Layout

```
v6emul/
├── Cargo.toml              # workspace root
├── Cargo.lock
├── design.md               # design spec (existing)
├── plan.md                 # this document
│
├── crates/
│   ├── v6core/             # emulator core library
│   │   ├── Cargo.toml
│   │   └── src/
│   │       ├── lib.rs
│   │       ├── cpu.rs          # Intel 8080 CPU emulation
│   │       ├── memory.rs       # 64K RAM + 8×256K RAM Disks
│   │       ├── io.rs           # I/O port subsystem (8255 PPI, timers, palette)
│   │       ├── display.rs      # scanline rasterizer (768×312 framebuffer)
│   │       ├── timer_i8253.rs  # i8253 programmable interval timer (3 counters)
│   │       ├── sound_ay8910.rs # AY-3-8910 sound chip
│   │       ├── fdc1793.rs      # WD1793 floppy disk controller
│   │       ├── keyboard.rs     # keyboard matrix scanning
│   │       ├── audio.rs        # audio mixing / downsampling (1.5MHz → 50kHz)
│   │       ├── hardware.rs     # orchestrator: emulation loop, timing, debug interface
│   │       └── breakpoint.rs   # breakpoint management (addr, conditions, page filter)
│   │
│   ├── v6ipc/              # IPC protocol and transport
│   │   ├── Cargo.toml
│   │   └── src/
│   │       ├── lib.rs
│   │       ├── protocol.rs     # message types, serialization (MessagePack)
│   │       ├── transport.rs    # shared-memory ring buffer + Unix/Named-pipe signaling
│   │       └── commands.rs     # typed command/response enums
│   │
│   └── v6cli/              # CLI binary
│       ├── Cargo.toml
│       └── src/
│           └── main.rs         # argument parsing, ROM loading, IPC server, test-output
│
└── tests/
    ├── cpu_tests.rs            # instruction-level unit tests
    ├── determinism_tests.rs    # replay / determinism verification
    ├── integration_tests.rs    # full ROM execution tests
    └── golden/                 # golden-file test fixtures (ROM + expected stdout)
```

### Rationale

| Crate | Role | Why separate? |
|-------|------|---------------|
| **v6core** | Pure emulator engine | Reusable by any frontend (ImGui, VS Code, tests) without pulling in I/O or CLI deps. No `std::thread`, no network code. |
| **v6ipc** | Transport + protocol | Isolates serialization format and transport choice; frontends only depend on this + v6core. |
| **v6cli** | Binary entry point | Thin wrapper: parses args, wires IPC transport to v6core, handles test-output mode. |

---

## 2. Runtime Model

### 2.1 Emulation Loop

The inner loop mirrors the C++ `Hardware::ExecuteInstruction()` cycle-accurate model:

```
loop {
    // Per machine cycle (4 T-states @ 3 MHz = 1.333 µs):
    display.rasterize();             // 4 pixels @ 12 MHz
    cpu.execute_machine_cycle();     // advance 1 machine cycle
    audio.clock(2);                  // 2 ticks @ 1.5 MHz

    io.try_commit(color_idx);        // deferred port commits

    if cpu.instruction_complete() {
        check_breakpoints();
        check_hlt_stop();            // --halt-exit mode
        handle_test_port_output();   // port 0xED capture
    }

    if display.frame_complete() {
        publish_frame();             // push to IPC ring buffer
        handle_pending_commands();   // process pause/step/reset
        throttle_to_50fps();         // sleep if real-time mode
    }
}
```

### 2.2 Timing Constants (from C++ source)

| Parameter | Value | Source |
|-----------|-------|--------|
| CPU clock | 3 MHz | hardware_consts.h |
| Display clock | 12 MHz (4× CPU) | display.h |
| Audio clock | 1.5 MHz (÷2 CPU) | audio.h |
| Cycles per frame | 59,904 (312 lines × 192 cycles) | display.h |
| Frame rate | 50.05 FPS (3 MHz ÷ 59,904) | display.h |
| VSYNC period | 19,968 µs | display.h |
| Scanline | 192 CPU cycles = 768 pixels | display.h |

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
│  (v6core::Hardware::run())        │    single-threaded, no Arc/Mutex on hot path
│  loops: cpu + display + audio     │
│  checks command channel each frame│
└──────────┬────────────────────────┘
           │ crossbeam channel (commands↓, events↑)
           │ shared-memory ring buffer (frames↑)
┌──────────┴────────────────────────┐
│          IPC Thread               │ ← v6ipc transport
│  reads external requests          │
│  writes responses + frames        │
└───────────────────────────────────┘
```

The emulation thread is the **sole owner** of core state — no interior mutability or locking on the hot path. Communication with the IPC thread uses lock-free channels (`crossbeam-channel`) for commands/events and a shared-memory ring buffer for frames.

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

- **Frame math**: 768 × 312 × 3 bytes (RGB24) = **719,424 bytes/frame** × 50 fps = **~34.3 MB/s**.
  TCP loopback can handle this, but shared memory eliminates the kernel copy entirely — critical for sustaining 50 Hz without jitter. The frame is written once into the ring buffer; the consumer reads it directly.

- **Cross-platform**: `shared_memory` crate wraps `mmap`/`CreateFileMapping`. Signaling via `interprocess` crate (named pipe on Windows, UDS on Unix).

- **Separation of concerns**: Large frame blits go through zero-copy shmem; small control messages (pause/step/registers) go through the named pipe with MessagePack encoding. This avoids head-of-line blocking.

#### Transport Layout

```
Shared Memory Region (e.g., 4 MB):
┌──────────────────────────────────────────┐
│ Header (64 bytes, cache-line aligned)    │
│   write_idx: AtomicU64                   │
│   read_idx:  AtomicU64                   │
│   frame_size: u32                        │
│   slot_count: u32 (e.g., 4 triple-buffer)│
├──────────────────────────────────────────┤
│ Slot 0: [u8; 719_424]  (frame 0)        │
│ Slot 1: [u8; 719_424]  (frame 1)        │
│ Slot 2: [u8; 719_424]  (frame 2)        │
│ Slot 3: [u8; 719_424]  (frame 3)        │
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
- Zero-alloc deserialization via `rmp-serde`.
- Mature Rust ecosystem.
- Easily consumed from TypeScript (`@msgpack/msgpack`) and C++ (`msgpack-c`).

### 4.2 Message Shape

Every message is a length-prefixed MessagePack blob:

```
[ u32 length ][ MessagePack payload ]
```

### 4.3 Command Messages (Client → Emulator)

```rust
#[derive(Serialize, Deserialize)]
pub enum Command {
    // Execution control
    Run,
    Pause,
    Step,                           // single instruction
    StepOver,                       // step over CALL
    StepFrame,                      // run one frame
    Reset,
    SetSpeed(ExecSpeed),

    // Memory / ROM
    LoadRom { addr: u16, data: Vec<u8> },
    ReadMemory { addr: u16, len: u16 },
    WriteMemory { addr: u16, data: Vec<u8> },

    // Registers
    GetRegisters,
    SetRegisters(CpuRegisters),

    // Breakpoints
    AddBreakpoint(Breakpoint),
    RemoveBreakpoint { id: u32 },
    ListBreakpoints,
    EnableBreakpoint { id: u32, enabled: bool },

    // Display
    RequestFrame,                   // one-shot frame request
    SubscribeFrames { enabled: bool }, // continuous 50 Hz streaming

    // I/O
    GetPortState,

    // Script / eval
    RunScript(String),

    // Lifecycle
    Ping,
    Shutdown,
}
```

### 4.4 Event Messages (Emulator → Client)

```rust
#[derive(Serialize, Deserialize)]
pub enum Event {
    // Execution state
    StateChanged(ExecState),        // Running, Paused, Halted
    BreakpointHit { id: u32, addr: u16 },
    StepComplete,

    // Data responses
    Registers(CpuRegisters),
    MemoryData { addr: u16, data: Vec<u8> },
    PortState(IoPortState),
    BreakpointList(Vec<Breakpoint>),

    // Frame (only via named pipe fallback; normally via shmem)
    Frame { seq: u64, data: Vec<u8> },
    FrameReady { seq: u64 },       // notification; data is in shmem

    // Test output
    TestOutput { port: u8, value: u8 },

    // Script result
    ScriptResult(String),

    // Lifecycle
    Pong,
    Error { code: u32, message: String },
}
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

**Location**: `crates/v6core/src/cpu.rs` (inline `#[cfg(test)]`) + `tests/cpu_tests.rs`

| Category | Approach |
|----------|----------|
| **All 256 opcodes** | Table-driven tests: for each opcode, set up initial registers/memory, execute, assert final state (registers, flags, memory, cycle count). |
| **Flag behavior** | Dedicated tests for carry, zero, sign, parity, aux-carry across arithmetic/logic ops. |
| **Interrupt handling** | Test EI/DI/RST sequences, verify interrupt vector dispatch. |
| **HLT** | Verify CPU halts and can be resumed by interrupt. |
| **Undocumented behavior** | Port known quirks from C++ (e.g., DAA edge cases). |

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

### 5.3 Determinism Tests

Run the same ROM with identical initial state twice; assert byte-identical frame output and cycle count. This catches any use of random state or uninitialized memory.

### 5.4 Integration Tests (ROM Execution)

- Load known-good ROM binaries (e.g., 8080EX1.COM, small demo programs).
- Run for N frames or until HLT.
- Assert final register/memory state or captured frame hash.

### 5.5 Golden Tests (Test-Port Output)

**Purpose**: Validate the `v6asm` test harness flow.

1. Assemble a small test ROM that performs `OUT 0xED, <value>` for each test assertion.
2. Run the emulator in `--halt-exit` mode.
3. Capture stdout.
4. Compare against a golden file (`tests/golden/<test_name>.expected`).

**Stdout format** (structured, machine-parseable):

```
TEST_OUT port=0xED value=0x01
TEST_OUT port=0xED value=0x00
...
HALT at PC=0x1234 after 12345 cycles
```

### 5.6 IPC Tests

- Round-trip: send `Command::Ping`, assert `Event::Pong`.
- Frame subscription: subscribe, run 3 frames, assert 3 `FrameReady` notifications.
- Breakpoint: set breakpoint, run ROM, assert `BreakpointHit`.
- Memory read/write round-trip.

---

## 6. Performance Considerations

### 6.1 Zero-Copy Frame Path

- The display rasterizer writes directly into a `Box<[u8; FRAME_LEN * 3]>` (RGB24).
- On frame completion, triple-buffer swap: `front ↔ back ↔ render`. No allocation.
- For IPC: the render buffer is `memcpy`'d into the shared-memory slot (one copy, ~0.7 ms for 700 KB at memory bandwidth). Alternative: rasterize directly into shmem if the consumer is fast enough.

### 6.2 Avoiding Allocations in the Hot Loop

- All per-instruction/per-cycle work uses stack-allocated or pre-allocated buffers.
- No `Vec` growth, no `Box::new`, no `String` formatting on the hot path.
- Breakpoint checking uses a pre-sorted `Vec<Breakpoint>` with binary search on PC, not a `HashMap`.

### 6.3 CPU Dispatch

- The 256-opcode dispatch table is a `[fn(&mut Cpu); 256]` array (direct function pointer lookup — one indirect call, no branching). This mirrors the C++ `switch` but avoids branch predictor pressure on large switch statements.
- Each instruction handler is `#[inline(never)]` to keep the dispatch loop's instruction cache footprint small.

### 6.4 Display Rasterizer

- Pre-bake the full 256-entry `vector_color → RGB` palette at init time (as in C++).
- Use `unsafe` for unchecked frame buffer indexing in the hot rasterize path (after validating bounds at frame start). Bounds checks in the inner pixel loop cost ~5% on benchmarks.
- Batch 16 pixels (4 CPU cycles) per rasterize call to amortize function-call overhead.

### 6.5 Audio Buffering

- Ring buffer of 4000 samples (~80 ms at 50 kHz).
- Downsampling from 1.5 MHz: accumulate 30 samples, average. No heap allocation.

### 6.6 IPC Throughput

- Shared memory ring buffer: 4 slots × 719,424 bytes ≈ 2.8 MB. Fits in L3 cache on most CPUs.
- Frame notification is a 12-byte message over named pipe (negligible latency).
- Control commands are small (<1 KB) and infrequent; MessagePack overhead is negligible.

---

## 7. Migration Strategy from C++

### 7.1 Component Mapping

| C++ Component | Rust Module | Notes |
|---------------|-------------|-------|
| `cpu_i8080.h/cpp` | `v6core::cpu` | 1:1 port; struct-of-arrays register layout. Port all 256 instruction handlers. |
| `memory.h/cpp` + `memory_consts.h` | `v6core::memory` | Port RAM Disk mapping logic. Replace raw pointers with `[u8; 65536]` + `[[u8; 262144]; 8]`. |
| `io.h/cpp` | `v6core::io` | Port commit timers. Replace C macros with struct field access. |
| `display.h/cpp` | `v6core::display` | Port scanline rasterizer. Replace `std::array<ColorI, N>` with `Box<[u8; N*3]>` (RGB24). |
| `timer_i8253.h` | `v6core::timer_i8253` | Direct port of 3-counter timer. |
| `sound_ay8910.h` | `v6core::sound_ay8910` | Direct port of AY chip register model + tone/noise/envelope. |
| `fdc_wd1793.h/cpp` | `v6core::fdc1793` | Port FDC state machine. Replace raw disk buffer pointers with `Vec<u8>`. |
| `keyboard.h/cpp` | `v6core::keyboard` | Port matrix scanning. Simplify (no SDL keycodes — accept abstract key events). |
| `audio.h/cpp` | `v6core::audio` | Port downsampler. Remove SDL dependency (output raw samples to ring buffer). |
| `hardware.h/cpp` + `hardware_consts.h` | `v6core::hardware` | Port execution loop. Replace `std::thread` + `std::mutex` with Rust channels. Replace C++ request enum with Rust `Command` enum. |
| `breakpoint.h/cpp` | `v6core::breakpoint` | Direct port. Replace `std::vector` with `Vec`. |

### 7.2 Incremental Validation Strategy

Port and validate components **bottom-up**, starting with leaf dependencies:

```
Phase 1: Foundation (no dependencies)
  ├── memory.rs     — unit tests for read/write/mapping
  ├── cpu.rs        — depends on memory (trait-abstracted)
  │                    validate with 8080 test suite ROMs
  └── keyboard.rs   — trivial; unit tests only

Phase 2: Peripherals
  ├── timer_i8253.rs   — unit tests for counter modes
  ├── sound_ay8910.rs  — unit tests for register R/W
  ├── fdc1793.rs       — unit tests with mock disk
  └── audio.rs         — unit tests for downsampling

Phase 3: I/O Orchestration
  ├── io.rs         — depends on keyboard, memory, timer, ay, fdc
  │                    integration tests: port IN/OUT sequences
  └── display.rs    — depends on memory, io
                       visual regression: render test patterns, compare hashes

Phase 4: Hardware Loop
  └── hardware.rs   — full integration
                       run 8080EX1.COM, verify test output
                       run demo ROMs, verify frame hashes

Phase 5: IPC + CLI
  ├── v6ipc         — protocol + transport tests
  └── v6cli         — end-to-end: CLI → IPC → run ROM → capture output
```

### 7.3 Behavior Drift Mitigation

1. **Cycle-count oracle**: Run the same instruction sequence in C++ Devector and Rust v6core; compare per-instruction cycle counts.
2. **Frame hash oracle**: Render the same ROM for N frames in both; compare SHA-256 of each frame.
3. **Test-port oracle**: Run the same test ROM in both; compare `OUT 0xED` sequences.

These oracles can be automated as CI jobs once Phase 4 is complete.

---

## 8. Dependency Summary

| Crate | Version | Purpose |
|-------|---------|---------|
| `clap` | 4.x | CLI argument parsing |
| `rmp-serde` | 1.x | MessagePack serialization |
| `serde` | 1.x | Serialization framework |
| `shared_memory` | 0.12+ | Cross-platform shared memory |
| `interprocess` | 2.x | Named pipes / UDS |
| `crossbeam-channel` | 0.5 | Lock-free MPSC channels |
| `log` + `env_logger` | 0.4 / 0.10 | Logging |
| `criterion` | 0.5 | Benchmarking |

No async runtime (tokio/async-std) — the emulation loop is synchronous and latency-sensitive. IPC threads use blocking I/O.

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
HALT at PC=0x0105 after 847231 cycles
```

This is consumed by test runners (e.g., `v6asm` integration tests) for deterministic assertion.

---

## 10. Implementation Order (Milestones)

### Milestone 1 — CPU + Memory (foundation)
- [ ] Workspace scaffolding (`Cargo.toml`, crate structure)
- [ ] `memory.rs`: 64K RAM + RAM Disk mapping + unit tests
- [ ] `cpu.rs`: full i8080 instruction set + cycle counting + unit tests
- [ ] Run 8080PRE.COM / 8080EX1.COM test ROMs to validate CPU

### Milestone 2 — Peripherals
- [ ] `keyboard.rs`: matrix scanning + unit tests
- [ ] `timer_i8253.rs`: 3-counter timer + unit tests
- [ ] `sound_ay8910.rs`: AY chip register model + unit tests
- [ ] `fdc1793.rs`: FDC state machine + unit tests (mock disk)
- [ ] `audio.rs`: downsampler + unit tests

### Milestone 3 — I/O + Display
- [ ] `io.rs`: port dispatch + commit timers + unit tests
- [ ] `display.rs`: scanline rasterizer + frame buffer + unit tests
- [ ] Integration test: render a test pattern ROM, verify frame hash

### Milestone 4 — Hardware Loop + Test Mode
- [ ] `hardware.rs`: emulation loop, timing, execution modes
- [ ] `--halt-exit` mode + port 0xED test output to stdout
- [ ] Golden tests: assemble test ROMs with v6asm, run, compare output
- [ ] Determinism test: run same ROM twice, assert identical output

### Milestone 5 — IPC
- [ ] `v6ipc::protocol`: MessagePack message types
- [ ] `v6ipc::transport`: shared-memory ring buffer + named pipe signaling
- [ ] `v6ipc::commands`: typed request/response API
- [ ] IPC integration tests (ping, frame subscribe, breakpoint)

### Milestone 6 — CLI + Polish
- [ ] `v6cli::main`: clap argument parsing, ROM loading, IPC server wiring
- [ ] TCP fallback transport
- [ ] End-to-end test: CLI → IPC → run ROM → capture frame
- [ ] Benchmarking with `criterion` (instructions/sec, frames/sec)
- [ ] CI pipeline (build + test on Linux/macOS/Windows)
