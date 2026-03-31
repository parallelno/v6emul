# Architecture

v6emul is a headless emulator for the **Vector-06C** (Вектор-06Ц), a Soviet 8-bit home computer from 1987. It runs as a TCP server that external frontends connect to for display, input, and debug.

## Library Structure

```
v6utils              ← independent (nlohmann::json only)
   ↑    ↑
v6core   v6ipc       ← both depend on v6utils; independent of each other
   ↑      ↑
     app             ← links all three into the v6emul binary
```

| Library | Role | External Deps |
|---------|------|---------------|
| **v6utils** | Shared types (`Addr`, `GlobalAddr`), `TQueue<T>`, `Result<T>`, JSON helpers, file I/O, `ArgsParser` | nlohmann::json |
| **v6core** | Full emulation engine + debug subsystem. No IPC knowledge. | LuaJIT |
| **v6ipc** | TCP transport + MessagePack protocol. No emulation knowledge. | platform sockets |
| **app** | CLI entry point. Wires IPC ↔ core. Test mode & server mode. | all three libs |

## Threading Model

Two threads with strict ownership:

| Thread | Entry Point | Role |
|--------|-------------|------|
| **Emulation thread** | `Hardware::Execution()` | Owns **all** mutable emulator state (CPU, Memory, Display, IO, Audio, FDC). Single-threaded hot path — no shared mutexes. Processes commands via `ReqHandling()`. |
| **Main thread** | `main()` | TCP server loop: accept → receive → deserialize MessagePack → `Hardware::Request()` → serialize response → send. In test mode, calls `RUN_HEADLESS` directly. |

**Cross-thread communication** uses `TQueue<T>` (mutex + condition variable):

- **Request queue**: `TQueue<pair<Req, json>>` — main thread pushes commands
- **Response queue**: `TQueue<json>` — emulation thread pushes results

`Hardware::Request()` is called from the main thread. It pushes a request and blocks until the emulation thread processes it and posts a response. All state mutation happens on the emulation thread.

## Key Classes

| Class | Header | Responsibility |
|-------|--------|----------------|
| `Hardware` | `core/hardware.h` | **Orchestrator**. Owns all subsystems. Runs the emulation loop. Exposes `Request(Req, json) → Result<json>` with ~96 commands. |
| `CpuI8080` | `core/cpu_i8080.h` | Intel 8080 (KR580VM80A) CPU at 3 MHz. Machine-cycle accurate (4 T-states/cycle). All 256 opcodes. Interrupt handling. |
| `Memory` | `core/memory.h` | 64 KB main RAM + 8 × 256 KB RAM Disks + ROM overlay. Address space translation. |
| `Display` | `core/display.h` | Cycle-accurate scanline rasterizer. 768×312 framebuffer (including borders). 50 fps PAL timing. |
| `IO` | `core/io.h` | I/O port dispatch. 8255 PPI. 16-color palette. Display mode switching. Keyboard scanning. FDC routing. |
| `TimerI8253` | `core/timer_i8253.h` | i8253 PIT — 3 counters, modes 0–5. |
| `SoundAY8910` | `core/sound_ay8910.h` | AY-3-8910 sound chip. Tone generators, noise, envelope. |
| `Audio` | `core/audio.h` | Mixes timer + AY + beeper. Downsamples 1.5 MHz → 50 KHz. Ring buffer output. |
| `Fdc1793` | `core/fdc_wd1793.h` | WD1793 floppy disk controller. Up to 4 drives. |
| `Keyboard` | `core/keyboard.h` | Abstract `KeyCode` enum → 8-row scan matrix. |
| `Scripts` | `core/scripts.h` | LuaJIT scripting engine for debug automation. |
| `Transport` | `ipc/transport.h` | Single-client TCP server. Length-prefixed framing. |

## Emulation Loop

```
Execution() {
  while (status != EXIT) {
    while (status == RUN) {
      ┌─ Frame ─────────────────────────────────────────────┐
      │  startFrameTime = now()                             │
      │                                                     │
      │  for each instruction in frame:                     │
      │    memory.DebugInit()                               │
      │    do {                                             │
      │      display.Rasterize()        // advance 4 pixels │
      │      cpu.ExecuteMachineCycle()   // 1 machine cycle  │
      │      audio.Clock(2, beeper)     // mix audio        │
      │    } while (!cpu.IsInstructionExecuted())           │
      │    Debug()  // if debugger attached                 │
      │    ReqHandling(0ns)  // non-blocking command poll   │
      │                                                     │
      │  // Vsync sleep                                     │
      │  while (now < endFrameTime)                         │
      │    ReqHandling(~200µs)  // serve commands in gap    │
      │                                                     │
      │  m_speedPercent = nominalFrameUs / totalFrameUs     │
      └─────────────────────────────────────────────────────┘
    }
    while (status == STOP) { ReqHandling(blocking) }
  }
}
```

**Frame timing** at normal speed: 19,968 µs per frame ≈ **50.08 fps**.
Each frame = 312 scanlines × 192 CPU cycles/scanline = **59,904 CPU cycles**.

Speed options: 1%, 20%, 50%, 100%, 200%, max (no delay).

## Memory Architecture

| Region | Size | Description |
|--------|------|-------------|
| Main RAM | 64 KB | Full address space `0x0000`–`0xFFFF` |
| RAM Disks | 8 × 256 KB (2 MB) | Each disk has 4 pages × 64 KB |
| ROM | variable | Overlaid on RAM, disabled via `RESTART` |
| **Total** | **~2.1 MB** | `MEMORY_MAIN_LEN + MEMORY_RAMDISK_LEN × RAM_DISK_MAX` |

**RAM Disk mapping** is controlled via port byte with bit layout `%E8ASssMM`:

- **Stack Mode** (`S` bit): RAM Disk accessed via stack instructions (PUSH/POP/CALL/RET/XTHL/RST)
- **Memory-Mapped Mode** (`8`/`A`/`E` bits): RAM Disk mapped into `0x8000–0x9FFF`, `0xA000–0xDFFF`, `0xE000–0xFFFF`
- `MM` = memory-mapped page index (0–3), `ss` = stack page index (0–3)

## Display

The rasterizer runs **cycle-accurately** — `Display::Rasterize()` is called every machine cycle (4 pixels per call). This enables pixel-perfect rendering of:

- Mid-scanline palette changes
- Mid-scanline scroll register commits
- Border colors
- MODE_256 and MODE_512 display modes

Frame buffer: 768 × 312 pixels, ABGR format (4 bytes/pixel). Delivered to frontends via pull-based `GET_FRAME` / `GET_FRAME_RAW` commands.

## Floppy Disk Controller

The `Fdc1793` class emulates the **KR1818WG93** (Soviet WD1793 clone):

- 4 drives max
- Disk geometry: 2 sides × 82 tracks × 5 sectors/track × 1024 bytes/sector = **819,200 bytes** per disk
- Ports: COMMAND/STATUS (0), TRACK (1), SECTOR (2), DATA (3), READY/SYSTEM (4)
- Loadable at runtime via `LOAD_FDD` command

## Lua Scripting

- **LuaJIT** linked via `ExternalProject_Add` in CMake
- Scripts execute on the **emulation thread** — direct pointer access to CPU, Memory, IO, and Display state
- `Scripts::Check()` is called from the debug callback (opt-in via `DEBUG_ATTACH`)
- Scripts can request UI rendering (text, rectangles) via `UIReqs`
- Managed through IPC commands: `DEBUG_SCRIPT_ADD`, `DEBUG_SCRIPT_DEL`, `DEBUG_SCRIPT_GET_ALL`, etc.

## Debug Subsystem

Debug is fully **opt-in** — activated via `DEBUG_ATTACH`. Features:

- **Breakpoints** — address-triggered, condition-based
- **Watchpoints** — memory read/write monitoring
- **Disassembler** — Intel 8080 and Z80 mnemonic modes
- **Trace log** — instruction-level execution recording
- **Recorder** — full state snapshot/rewind (time travel debugging)
- **Code performance** — cycle counting for address ranges
- **Memory edits** — runtime byte patches
- **Lua scripts** — programmable debug automation

## Test Mode

Invoked via `--halt-exit`, `--run-frames`, or `--run-cycles` flags. Runs headlessly via `RunHeadless()` — a tight loop without frame timing or vsync. Captures `OUT 0xED` for test assertions, printing results to stdout for golden-file comparison.
