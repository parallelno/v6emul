# IPC Protocol

v6emul communicates with external frontends over **TCP loopback** (`127.0.0.1`). The protocol is pure request/response — the client sends a request, the emulator replies with exactly one response.

## Wire Format

All messages use **length-prefixed MessagePack** framing:

```
[4 bytes: uint32_t payload length, little-endian] [N bytes: MessagePack payload]
```

The payload is serialized using `nlohmann::json::to_msgpack()` / `from_msgpack()`.

### Request Format

```json
{
  "cmd": <int>,
  "data": { ... }
}
```

- `cmd` — command identifier (see [Command Reference](#command-reference) below)
- `data` — command-specific object; it may be omitted or `{}` for commands that take no arguments

The server rejects non-object requests, non-integer command IDs, unknown commands, and non-object `data` before dispatching to the emulation thread.

### Response Format

```json
{
  "ok": true,
  "data": { ... }
}
```

On error:

```json
{
  "ok": false,
  "code": "invalid_request",
  "error": "description",
  "details": {"command": 69, "field": "len"}
}
```

Error codes are stable for client branching; `error` is a human-readable diagnostic. `details` is present when the server can identify a command-specific failing field:

| Code | Meaning |
|------|---------|
| `decode_error` | The MessagePack payload could not be decoded |
| `invalid_request` | The request envelope or command data is invalid |
| `unknown_command` | The command ID is not supported |
| `dispatch_error` | Command processing failed after validation |
| `internal_error` | An unclassified server error |

## Special Commands (Pseudo-Commands)

These use negative `cmd` values and are handled directly by the IPC server layer, not routed through `Hardware::Request()`.

| cmd | Name | Description |
|-----|------|-------------|
| `-1` | `PING` | Health check. Returns `{"pong": true}` |
| `-3` | `GET_FRAME` | Returns ABGR frame buffer as MessagePack binary |
| `-4` | `GET_FRAME_RAW` | Returns raw binary frame (bypasses MessagePack, see below) |
| `-5` | `GET_SERVER_INFO` | Returns protocol, build, command, and capability information |

### GET_SERVER_INFO Response

Clients must call `GET_SERVER_INFO` during connection setup and require `protocolVersion = 2` plus the capabilities they use before issuing other commands. The server implements protocol version 2 only.

```json
{
  "ok": true,
  "data": {
    "protocolVersion": 2,
    "emulatorVersion": "2026.07.31-07dba54",
    "commands": [-5, -4, -3, -1, 1, 2],
    "capabilities": {
      "debugger": true,
      "rawFrame": true,
      "rawFrameSchema": 1,
      "stackSampleSchema": 1,
      "stopRecordSchema": 1
    }
  }
}
```

`commands` contains the complete list for the running binary; the shortened list above is illustrative. `emulatorVersion` is the same build identity printed by `v6emul --version`.

### GET_FRAME Response

Standard MessagePack response containing:

```json
{
  "ok": true,
  "data": {
    "width": 768,
    "height": 312,
    "pixels": <binary: ABGR pixel data>
  }
}
```

### GET_FRAME_RAW Response

For high-throughput frame streaming, this command returns a fixed binary envelope that bypasses MessagePack encoding. Success and failure use the same framing and can always be distinguished by `kind`:

```
[4 bytes: payloadLen] [16 bytes: header] [payloadLen-16 bytes: body]
```

All multi-byte integers are unsigned and little-endian. The 16-byte payload header is:

| Offset | Size | Field | Value |
|--------|------|-------|-------|
| 0 | 4 | `magic` | ASCII `V6RF` |
| 4 | 1 | `schemaVersion` | `1` |
| 5 | 1 | `kind` | `1` = frame, `2` = error |
| 6 | 2 | `flags` | Reserved; must be zero |
| 8 | 4 | `value0` | Kind-specific, described below |
| 12 | 4 | `value1` | Kind-specific, described below |

For `kind = 1` (frame), `value0` is width, `value1` is height, and the body contains exactly `width * height * 4` pixel bytes.

For `kind = 2` (error), `value0` is the numeric error code, `value1` is the UTF-8 message length, and the body contains exactly that message. Defined error codes:

| Code | Name | Meaning |
|------|------|---------|
| 1 | `FRAME_UNAVAILABLE` | No frame is currently available |
| 2 | `INTERNAL_ERROR` | The server could not encode the frame |

- Frame dimensions: 768 × 312 pixels
- Pixel format depends on `--color-format`:
  - `abgr` (default) — bytes `[R, G, B, A]` per pixel. Native for HTML Canvas `ImageData`, WebGL, and most graphics APIs.
  - `argb` — bytes `[B, G, R, A]` per pixel. Native for Windows `BI_RGB` bitmaps (GDI `StretchDIBits`).
- Total pixel data: 768 × 312 × 4 = 958,464 bytes

`GET_FRAME_RAW` supports only this protocol-v2 envelope. There is no legacy raw-frame response mode or fallback parser.

## Hardware Commands

Positive `cmd` values map directly to the `Hardware::Req` enum. These are dispatched through `Hardware::Request()` on the emulation thread via the thread-safe command queue.

### Emulation Control

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 1 | `RUN` | — | — |
| 2 | `STOP` | — | — |
| 3 | `IS_RUNNING` | — | `{"isRunning": bool}` |
| 4 | `EXIT` | — | `{"exiting": true}` (server shuts down) |
| 5 | `RESET` | — | — (reboot, enable ROM) |
| 6 | `RESTART` | — | — (reboot, disable ROM) |
| 7 | `EXECUTE_INSTR` | — | — (single instruction step) |
| 8 | `EXECUTE_FRAME` | — | — (run one full frame) |
| 9 | `EXECUTE_FRAME_NO_BREAKS` | — | — (run one full frame ignoring breakpoints) |
| 44 | `SET_CPU_SPEED` | `{"speed": int}` | — |
| 50 | `RUN_HEADLESS` | `{"haltExit": bool, "maxFrames": int, "maxCycles": int}` | `{"cc", "frames", "halted", "pc", "sp", "af", "bc", "de", "hl"}` |
| 95 | `GET_STOP_RECORD` | — | latest unified stop record |

### Stop Record Schema 1

`GET_STOP_RECORD` is non-consuming: repeated reads return the same object until a newer stop event replaces it. Before the first stop, it returns sequence `0`, reason `unknown`, and the initial PC/global instruction address. Sequences increase for every recorded stop for the lifetime of the emulator process. Reset, restart, ROM auto-boot, attaching, reconnecting, and loading a ROM do not alter the sequence or latest record because none of those operations stops emulation.

Every response contains `sequence`, `reason`, `pc`, and `globalInstructionAddress`. Reasons are `pause`, `breakpoint`, `watchpoint`, `step`, `next`, `frameStep`, `exception`, or `unknown`. Trigger-specific fields are omitted when they do not apply:

- Breakpoints add `breakpointIds`, `breakpointAddress`, and optionally `description`. Breakpoint addresses are the stable breakpoint identities used in `breakpointIds`.
- Watchpoints add sorted `watchpointIds`, `access` (`read` or `write`), `accessedGlobalAddress`, and `description`. Reads add `observedValue`; writes add `oldValue` and `newValue` when available.
- Exceptions may add `exceptionCode` and `description`.

Manual `STOP`, debugger hits, single-instruction and frame steps, and emulation exceptions each replace the record atomically on the emulation thread. `HLT` changes CPU state but does not stop the emulator; reset and restart reinitialize hardware without changing run/stop status. Therefore these operations do not create stop records. Connection loss and process exit are transport/process lifecycle events and are not stop records.

Speed values for `SET_CPU_SPEED`:

| Value | Speed |
|-------|-------|
| 0 | 1% |
| 1 | 20% |
| 2 | 50% |
| 3 | 100% (normal) |
| 4 | 200% |
| 5 | max |

### CPU State

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 10 | `GET_CC` | — | `{"cc": uint64}` |
| 11 | `GET_REGS` | — | `{"cc", "pc", "sp", "af", "bc", "de", "hl", "ints", "m"}` |
| 12 | `GET_REG_PC` | — | `{"pc": uint16}` |

### Memory Access

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 13 | `GET_BYTE_GLOBAL` | `{"globalAddr": int}` | `{"data": uint8}` |
| 14 | `GET_BYTE_RAM` | `{"addr": int}` | `{"data": uint8}` |
| 15 | `GET_THREE_BYTES_RAM` | `{"addr": int}` | `{"data": int}` |
| 16 | `GET_MEM_STRING_GLOBAL` | `{"addr": int, "len": int}` | `{"data": string}` |
| 17 | `GET_WORD_STACK` | `{"addr": int}` | `{"data": uint16}` |
| 18 | `GET_STACK_SAMPLE` | `{"addr": uint16}` | Object containing 11 words keyed by offsets `-10` through `10` |
| 42 | `SET_MEM` | `{"addr": int, "data": [bytes]}` | — |
| 43 | `SET_BYTE_GLOBAL` | `{"addr": int, "data": uint8}` | — |
| 93 | `GET_MEM` | `{"addr": uint32, "len": uint32}` | `{"addr": uint32, "data": [bytes]}` |

#### GET_MEM (cmd 93)

`GET_MEM` reads a non-empty range from the global memory address space. `addr` is a global address, not a 16-bit CPU address. The range uses an inclusive start and exclusive end: `addr + len` must be less than or equal to `MEMORY_GLOBAL_LEN`.

Requests with a missing field, a non-integer or negative value, `len` equal to zero, an address outside global memory, or a range that crosses the end of global memory are rejected with an `invalid_request` error.

```json
{
  "cmd": 93,
  "data": {
    "addr": 1234,
    "len": 16
  }
}
```

The response contains the requested bytes in address order:

```json
{
  "ok": true,
  "data": {
    "addr": 1234,
    "data": [0, 1, 2, 3]
  }
}
```

#### GET_STACK_SAMPLE (cmd 18)

`addr` is required and must be an integer from `0` through `65535`. It normally contains the current stack pointer. The command is valid while paused or running.

Each response value is a little-endian 16-bit word beginning at `addr + offset`. Address arithmetic wraps modulo 65536, so a sample around `0x0000` reads negative offsets from the top of the 16-bit address space.

```json
{
  "ok": true,
  "data": {
    "-10": 4660,
    "-8": 22136,
    "-6": 39612,
    "-4": 57072,
    "-2": 4951,
    "0": 9320,
    "2": 13980,
    "4": 18640,
    "6": 23300,
    "8": 27960,
    "10": 32620
  }
}
```

### Display

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 19 | `GET_DISPLAY_DATA` | — | `{"rasterLine", "rasterPixel", "frameNum"}` |
| 26 | `GET_RUSLAT_HISTORY` | — | `{"data": int}` |
| 27 | `GET_SCROLL_VERT` | — | `{"scrollVert": int}` |
| 36 | `GET_DISPLAY_BORDER_LEFT` | — | `{"borderLeft": int}` |
| 37 | `SET_DISPLAY_BORDER_LEFT` | `{"borderLeft": int}` | — |
| 38 | `GET_DISPLAY_IRQ_COMMIT_PXL` | — | `{"irqCommitPxl": int}` |
| 39 | `SET_DISPLAY_IRQ_COMMIT_PXL` | `{"irqCommitPxl": int}` | — |
| 40 | `SET_FRAME_MODE` | `{"frameMode": int}` | — |
| 41 | `SET_COLOR_FORMAT` | `{"colorFormat": int}` | — |

### I/O & Palette

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 29 | `GET_IO_PORTS` | — | `{"data": int}` |
| 30 | `GET_IO_PORTS_IN_DATA` | — | `{"bytes": binary(256)}` |
| 31 | `GET_IO_PORTS_OUT_DATA` | — | `{"bytes": binary(256)}` |
| 32 | `GET_IO_DISPLAY_MODE` | — | `{"data": int}` |
| 33 | `GET_IO_PALETTE` | — | `{"low", "hi"}` |
| 34 | `GET_IO_PALETTE_COMMIT_TIME` | — | `{"paletteCommitTime": int}` |
| 35 | `SET_IO_PALETTE_COMMIT_TIME` | `{"paletteCommitTime": int}` | — |
| 97 | `SET_IO_PALETTE_ENTRY` | `{"index": 0..15, "hwColor": 0..255}` | echoes `index` and `hwColor` |

`SET_IO_PALETTE_ENTRY` accepts exactly the two fields shown, requires stopped hardware, and updates the selected raw `BBGGGRRR` palette byte immediately without changing palette commit timing.

### Memory Mapping

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 20 | `GET_MEMORY_MAPPING` | — | `{"mapping", "ramdiskIdx"}` |
| 21 | `GET_MEMORY_MAPPINGS` | — | `{"ramdiskIdx", "mapping0"..."mapping7"}` |
| 22 | `GET_GLOBAL_ADDR_RAM` | `{"addr": int}` | `{"data": int}` |
| 28 | `GET_STEP_OVER_ADDR` | — | `{"data": int}` |
| 46 | `IS_MEMROM_ENABLED` | — | `{"data": bool}` |

### Hardware Stats

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 45 | `GET_HW_MAIN_STATS` | — | See below |
| 96 | `GET_HARDWARE_STATS` | `{}` | Hardware statistics schema 1 |

`GET_HW_MAIN_STATS` returns:

```json
{
  "cc": <uint64>,
  "rasterLine": <int>,
  "rasterPixel": <int>,
  "frameCc": <int>,
  "frameNum": <uint64>,
  "displayMode": <int>,
  "scrollVert": <int>,
  "rusLat": <bool>,
  "inte": <bool>,
  "iff": <bool>,
  "hlta": <bool>,
  "speedPercent": <double>,
  "palette0"..."palette15": <uint32>
}

```

`GET_HARDWARE_STATS` is the non-legacy schema and is valid while hardware is stopped or running. It is captured by one request on the emulation thread; while running, dispatch occurs at an instruction boundary without stopping execution. The response contains session identity/uptime, monotonic `cpuCycles`, latched `lastRunCycles`, raster/frame state, CPU interrupt state, exactly 16 raw palette bytes, RAM-disk mapping, and exactly four FDD entries. `GET_SERVER_INFO` advertises this behavior as `hardwareStatsWhileRunning: true`.

Counters survive `RESET` and `RESTART` within a connection and reset when a new client session begins. `lastRunCycles` is latched only when running execution stops, so repeated stopped reads return the same value. All counters are 64-bit and requests fail before a value exceeds JavaScript's maximum safe integer; a future bigint schema is required beyond that limit.

### FDC / Floppy

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 23 | `GET_FDC_INFO` | — | `{"drive", "side", "track", "lastS", "wait", "cmd", "rwLen", "position"}` |
| 24 | `GET_FDD_INFO` | `{"driveIdx": int}` | `{"path", "updated", "reads", "writes", "mounted"}` |
| 25 | `GET_FDD_IMAGE` | `{"driveIdx": int}` | `{"data": [bytes]}` |
| 48 | `LOAD_FDD` | `{"driveIdx": int, "data": [bytes], "path": string}` | — |
| 49 | `RESET_UPDATE_FDD` | `{"driveIdx": int}` | — |
| 91 | `LOAD_ROM` | `{"data": [bytes], "addr": int, "autorun": bool}` | — |
| 92 | `MOUNT_FDD` | `{"data": [bytes], "driveIdx": int, "path": string, "autoBoot": bool}` | — |
| 98 | `DISMOUNT_FDD` | `{"driveIdx": 0..3}` | `{"driveIdx": int, "mounted": false}` |

#### LOAD_ROM (cmd 91)

High-level ROM loading command. Stops emulation, writes `data` into RAM starting at `addr`, performs a `RESTART` (disables ROM overlay, resets CPU), and optionally starts running.

- `data` — raw ROM bytes
- `addr` — load address (default `0`)
- `autorun` — if `true`, starts emulation after loading (default `false`)

#### MOUNT_FDD (cmd 92)

High-level floppy disk mounting command. Pads/truncates `data` to the standard FDD size (819,200 bytes), mounts it on the specified drive, and optionally resets the machine to boot from disk.

- `data` — raw disk image bytes
- `driveIdx` — drive index 0–3 (default `0`)
- `path` — original file path for display purposes
- `autoBoot` — if `true`, performs `RESET` (enables boot ROM) and starts emulation (default `false`)

#### DISMOUNT_FDD (cmd 98)

Requires stopped hardware and accepts exactly `driveIdx`. Dismounting an empty drive is idempotent. It clears mounted state and path without saving or discarding dirty disk bytes. Dismounting the selected drive leaves the selected-drive register unchanged and makes subsequent reads behave as no media present.

#### FDD Persistence Workflow

To implement save/discard for modified floppy disks:

1. **Poll for changes**: Send `GET_FDD_INFO` with `{"driveIdx": N}`. Check the `updated` field — `true` means the disk has been written to.
2. **Export disk image**: Send `GET_FDD_IMAGE` with `{"driveIdx": N}`. Returns the full 819,200-byte image in `data`.
3. **Save to file**: Write the exported bytes to disk (client-side).
4. **Clear dirty flag**: Send `RESET_UPDATE_FDD` with `{"driveIdx": N}` to mark the disk as clean.

### Keyboard

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 47 | `KEY_HANDLING` | `{"scancode": int, "action": int}` | — |

### Debug: Breakpoints

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 60 | `DEBUG_BREAKPOINT_ADD` | structured breakpoint definition | — |
| 61 | `DEBUG_BREAKPOINT_DEL` | `{"addr": uint16}` | — |
| 62 | `DEBUG_BREAKPOINT_DEL_ALL` | — | — |
| 63 | `DEBUG_BREAKPOINT_GET_STATUS` | `{"addr": uint16}` | `{"status": string}` |
| 64 | `DEBUG_BREAKPOINT_SET_STATUS` | `{"addr": uint16, "status": string}` | — |
| 65 | `DEBUG_BREAKPOINT_ACTIVE` | `{"addr": uint16}` | — |
| 66 | `DEBUG_BREAKPOINT_DISABLE` | `{"addr": uint16}` | — |
| 67 | `DEBUG_BREAKPOINT_GET_ALL` | — | structured breakpoint array |
| 68 | `DEBUG_BREAKPOINT_GET_UPDATES` | — | `{"updates": uint32}` |

#### Structured Breakpoint Schema 1

The breakpoint protocol uses structured schema 1 exclusively. Adding an existing address replaces its configuration:

```json
{
  "addr": 4660,
  "memPages": 8589934591,
  "status": "ACTIVE",
  "autoDelete": false,
  "operand": "A",
  "condition": "ANY",
  "value": 0,
  "comment": "interrupt entry"
}
```

| Field | Constraint |
|-------|------------|
| `addr` | Unsigned 16-bit CPU address |
| `memPages` | Non-zero 33-bit mapping mask |
| `status` | `ACTIVE` or `DISABLED` |
| `autoDelete` | Boolean |
| `operand` | `A`, `F`, `B`, `C`, `D`, `E`, `H`, `L`, `PSW`, `BC`, `DE`, `HL`, `CC`, or `SP` |
| `condition` | `ANY`, `EQU`, `LESS`, `GREATER`, `LESS_EQU`, `GREATER_EQU`, or `NOT_EQU` |
| `value` | Unsigned; 8-bit for byte operands, 16-bit for word operands, and 64-bit for `CC` |
| `comment` | UTF-8 string, at most 1024 encoded bytes |

In `memPages`, bit 0 selects main RAM. Bit `1 + 4 * ramDisk + page` selects one of four pages in RAM disks 0 through 7. `8589934591` (`0x1FFFFFFFF`) selects every mapping. Comparisons are unsigned; `ANY` ignores `value`.

`DEBUG_BREAKPOINT_GET_ALL` always returns an array ordered by ascending `addr`; an empty collection is `[]`. Packed breakpoint words are not accepted or returned. `DEBUG_BREAKPOINT_GET_STATUS` returns `ACTIVE`, `DISABLED`, or `DELETED`, where `DELETED` means no breakpoint exists at the address. Status mutation accepts only `ACTIVE` or `DISABLED`.

`DEBUG_BREAKPOINT_GET_UPDATES` is a 32-bit unsigned wrapping mutation counter. Adds, replacements, effective status changes, auto-deletions, and effective deletes increment it. Rejected requests and no-op mutations do not.

Support and limits are advertised by `GET_SERVER_INFO` under `breakpointSchema` and `breakpointLimits`.

### Debug: Watchpoints

| cmd | Name | Data |
|-----|------|------|
| 69 | `DEBUG_WATCHPOINT_ADD` | structured watchpoint definition |
| 70 | `DEBUG_WATCHPOINT_DEL_ALL` | — |
| 71 | `DEBUG_WATCHPOINT_DEL` | watchpoint id |
| 72 | `DEBUG_WATCHPOINT_GET_UPDATES` | — |
| 73 | `DEBUG_WATCHPOINT_GET_ALL` | — |
| 94 | `DEBUG_WATCHPOINT_EDIT` | structured watchpoint definition plus `id` |

#### Structured Watchpoint Schema 1

The watchpoint protocol uses structured schema 1 exclusively. An add request contains configuration only; the server allocates the ID:

```json
{
  "globalAddr": 65536,
  "len": 4,
  "value": 32,
  "access": "RW",
  "condition": "EQU",
  "type": "LEN",
  "active": true,
  "comment": "screen buffer"
}
```

`DEBUG_WATCHPOINT_GET_ALL` returns an array of the same fields plus `id`, ordered by ascending ID. An empty collection is `[]`. Runtime match state is not serialized. Packed watchpoint representations are not accepted or returned.

`DEBUG_WATCHPOINT_EDIT` requires every schema field plus the non-negative `id` returned by `DEBUG_WATCHPOINT_GET_ALL`. It replaces that watchpoint's configuration while preserving the ID. An unknown ID returns `invalid_request` with `details.command = 94`, `details.field = "id"`, and the rejected ID. Failed edits do not mutate the collection or increment its update counter.

Schema 1 constraints:

| Field | Constraint |
|-------|------------|
| `globalAddr` | Unsigned byte address less than `MEMORY_GLOBAL_LEN` |
| `len` | Positive; the complete range must fit in global memory |
| `value` | `0..255` for `LEN`; `0..65535` for `WORD` |
| `access` | `R`, `W`, or `RW` |
| `condition` | `ANY`, `EQU`, `LESS`, `GREATER`, `LESS_EQU`, `GREATER_EQU`, or `NOT_EQU` |
| `type` | `LEN` or `WORD` |
| `active` | Boolean |
| `comment` | UTF-8 string, at most 1024 encoded bytes |

Unknown fields are rejected. `WORD` requires `len = 2` and compares the low byte at `globalAddr` and high byte at `globalAddr + 1`. Comparisons are unsigned. `ANY` ignores `value`. `LEN` evaluates each accessed byte independently against the low byte of `value`. Read and write accesses are classified separately; `RW` matches either. A `WORD` may accumulate its low/high matches across instructions until a watchpoint stop resets match state.

`DEBUG_WATCHPOINT_GET_UPDATES` returns a 32-bit unsigned wrapping mutation counter. Successful adds and effective deletes increment it. Rejected requests, deleting an unknown ID, and clearing an already empty collection do not. Watchpoint mutations are serialized on the emulation thread and may be requested while execution is running.

Structured schema support and limits are advertised by `GET_SERVER_INFO` under `watchpointSchema`, `watchpointServerAllocatedIds`, `watchpointEdit`, `watchpointMutationsWhileRunning`, and `watchpointLimits`.

### Debug: Memory Edits

| cmd | Name | Data |
|-----|------|------|
| 74 | `DEBUG_MEMORY_EDIT_ADD` | edit definition |
| 75 | `DEBUG_MEMORY_EDIT_DEL_ALL` | — |
| 76 | `DEBUG_MEMORY_EDIT_DEL` | edit id |
| 77 | `DEBUG_MEMORY_EDIT_GET` | edit id |
| 78 | `DEBUG_MEMORY_EDIT_EXISTS` | edit id |

### Debug: Code Performance

| cmd | Name | Data |
|-----|------|------|
| 79 | `DEBUG_CODE_PERF_ADD` | perf region definition |
| 80 | `DEBUG_CODE_PERF_DEL_ALL` | — |
| 81 | `DEBUG_CODE_PERF_DEL` | perf region id |
| 82 | `DEBUG_CODE_PERF_GET` | perf region id |
| 83 | `DEBUG_CODE_PERF_EXISTS` | perf region id |

### Debug: Lua Scripts

| cmd | Name | Data |
|-----|------|------|
| 84 | `DEBUG_SCRIPT_ADD` | script definition |
| 85 | `DEBUG_SCRIPT_DEL_ALL` | — |
| 86 | `DEBUG_SCRIPT_DEL` | script id |
| 87 | `DEBUG_SCRIPT_GET_ALL` | — |
| 88 | `DEBUG_SCRIPT_GET_UPDATES` | — |

### Debug: Recorder

| cmd | Name |
|-----|------|
| 53 | `DEBUG_RECORDER_RESET` |
| 54 | `DEBUG_RECORDER_PLAY_FORWARD` |
| 55 | `DEBUG_RECORDER_PLAY_REVERSE` |
| 56 | `DEBUG_RECORDER_GET_STATE_RECORDED` |
| 57 | `DEBUG_RECORDER_GET_STATE_CURRENT` |
| 58 | `DEBUG_RECORDER_SERIALIZE` |
| 59 | `DEBUG_RECORDER_DESERIALIZE` |

### Debug: Trace Log

| cmd | Name |
|-----|------|
| 89 | `DEBUG_TRACE_LOG_ENABLE` |
| 90 | `DEBUG_TRACE_LOG_DISABLE` |

### Debug: Other

| cmd | Name | Data |
|-----|------|------|
| 51 | `DEBUG_ATTACH` | `{"data": bool}` |
| 52 | `DEBUG_RESET` | — |

## Throughput

- Frame size: 768 × 312 × 4 bytes = 958,464 bytes
- At 50 fps: ~48 MB/s
- TCP loopback throughput: ~700 MB/s
- Headroom: ~14×

The `GET_FRAME_RAW` command bypasses MessagePack serialization for frame data. Its 20-byte wire prefix consists of the common 4-byte payload length and a 16-byte typed header.
