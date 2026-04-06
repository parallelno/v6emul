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
- `data` — command-specific parameters (may be `null` or `{}` for commands that take no arguments)

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
  "error": "description"
}
```

## Special Commands (Pseudo-Commands)

These use negative `cmd` values and are handled directly by the IPC server layer, not routed through `Hardware::Request()`.

| cmd | Name | Description |
|-----|------|-------------|
| `-1` | `PING` | Health check. Returns `{"pong": true}` |
| `-3` | `GET_FRAME` | Returns ABGR frame buffer as MessagePack binary |
| `-4` | `GET_FRAME_RAW` | Returns raw binary frame (bypasses MessagePack, see below) |

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

For high-throughput frame streaming, this command returns a **raw binary** response that bypasses MessagePack encoding:

```
[4 bytes: payloadLen (uint32_t)] [4 bytes: width] [4 bytes: height] [payloadLen-8 bytes: raw pixels]
```

- Frame dimensions: 768 × 312 pixels
- Pixel format depends on `--frame-format`:
  - `rgba` (default) — bytes `[R, G, B, A]` per pixel. Native for HTML Canvas `ImageData`, WebGL, and most graphics APIs.
  - `bgra` — bytes `[B, G, R, A]` per pixel. Native for Windows `BI_RGB` bitmaps (GDI `StretchDIBits`).
- Total pixel data: 768 × 312 × 4 = 958,464 bytes

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
| 9 | `EXECUTE_FRAME_NO_BREAKS` | — | — (run one full frame ignoring breakpoints) |
| 42 | `SET_CPU_SPEED` | `{"speed": int}` | — |
| 49 | `RUN_HEADLESS` | `{"haltExit": bool, "maxFrames": int, "maxCycles": int}` | `{"cc", "frames", "halted", "pc", "sp", "af", "bc", "de", "hl"}` |

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
| 18 | `GET_STACK_SAMPLE` | `{"addr": int}` | 11 word values at offsets -10 to +10 |
| 40 | `SET_MEM` | `{"addr": int, "data": [bytes]}` | — |
| 41 | `SET_BYTE_GLOBAL` | `{"addr": int, "data": uint8}` | — |

### Display

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 19 | `GET_DISPLAY_DATA` | — | `{"rasterLine", "rasterPixel", "frameNum"}` |
| 27 | `GET_SCROLL_VERT` | — | `{"scrollVert": int}` |
| 36 | `GET_DISPLAY_BORDER_LEFT` | — | `{"borderLeft": int}` |
| 37 | `SET_DISPLAY_BORDER_LEFT` | `{"borderLeft": int}` | — |
| 38 | `GET_DISPLAY_IRQ_COMMIT_PXL` | — | `{"irqCommitPxl": int}` |
| 39 | `SET_DISPLAY_IRQ_COMMIT_PXL` | `{"irqCommitPxl": int}` | — |

### I/O & Palette

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 29 | `GET_IO_PORTS` | — | `{"data": int}` |
| 30 | `GET_IO_PORTS_IN_DATA` | — | `{"data0"..."data7"}` |
| 31 | `GET_IO_PORTS_OUT_DATA` | — | `{"data0"..."data7"}` |
| 32 | `GET_IO_DISPLAY_MODE` | — | `{"data": int}` |
| 33 | `GET_IO_PALETTE` | — | `{"low", "hi"}` |
| 34 | `GET_IO_PALETTE_COMMIT_TIME` | — | `{"paletteCommitTime": int}` |
| 35 | `SET_IO_PALETTE_COMMIT_TIME` | `{"paletteCommitTime": int}` | — |

### Memory Mapping

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 20 | `GET_MEMORY_MAPPING` | — | `{"mapping", "ramdiskIdx"}` |
| 21 | `GET_MEMORY_MAPPINGS` | — | `{"ramdiskIdx", "mapping0"..."mapping7"}` |
| 22 | `GET_GLOBAL_ADDR_RAM` | `{"addr": int}` | `{"data": int}` |
| 44 | `IS_MEMROM_ENABLED` | — | `{"data": bool}` |

### Hardware Stats

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 43 | `GET_HW_MAIN_STATS` | — | See below |

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

### FDC / Floppy

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 23 | `GET_FDC_INFO` | — | `{"drive", "side", "track", "lastS", "wait", "cmd", "rwLen", "position"}` |
| 24 | `GET_FDD_INFO` | `{"driveIdx": int}` | `{"path", "updated", "reads", "writes", "mounted"}` |
| 25 | `GET_FDD_IMAGE` | `{"driveIdx": int}` | `{"data": [bytes]}` |
| 46 | `LOAD_FDD` | `{"driveIdx": int, "data": [bytes], "path": string}` | — |
| 47 | `RESET_UPDATE_FDD` | `{"driveIdx": int}` | — |
| 89 | `LOAD_ROM` | `{"data": [bytes], "addr": int, "autorun": bool}` | — |
| 90 | `MOUNT_FDD` | `{"data": [bytes], "driveIdx": int, "path": string, "autoBoot": bool}` | — |

#### LOAD_ROM (cmd 89)

High-level ROM loading command. Stops emulation, writes `data` into RAM starting at `addr`, performs a `RESTART` (disables ROM overlay, resets CPU), and optionally starts running.

- `data` — raw ROM bytes
- `addr` — load address (default `0`)
- `autorun` — if `true`, starts emulation after loading (default `false`)

#### MOUNT_FDD (cmd 90)

High-level floppy disk mounting command. Pads/truncates `data` to the standard FDD size (819,200 bytes), mounts it on the specified drive, and optionally resets the machine to boot from disk.

- `data` — raw disk image bytes
- `driveIdx` — drive index 0–3 (default `0`)
- `path` — original file path for display purposes
- `autoBoot` — if `true`, performs `RESET` (enables boot ROM) and starts emulation (default `false`)

#### FDD Persistence Workflow

To implement save/discard for modified floppy disks:

1. **Poll for changes**: Send `GET_FDD_INFO` with `{"driveIdx": N}`. Check the `updated` field — `true` means the disk has been written to.
2. **Export disk image**: Send `GET_FDD_IMAGE` with `{"driveIdx": N}`. Returns the full 819,200-byte image in `data`.
3. **Save to file**: Write the exported bytes to disk (client-side).
4. **Clear dirty flag**: Send `RESET_UPDATE_FDD` with `{"driveIdx": N}` to mark the disk as clean.

### Keyboard

| cmd | Name | Data | Response |
|-----|------|------|----------|
| 45 | `KEY_HANDLING` | `{"scancode": int, "action": int}` | — |

### Debug: Breakpoints

| cmd | Name | Data |
|-----|------|------|
| 58 | `DEBUG_BREAKPOINT_ADD` | breakpoint definition |
| 59 | `DEBUG_BREAKPOINT_DEL` | breakpoint id |
| 60 | `DEBUG_BREAKPOINT_DEL_ALL` | — |
| 61 | `DEBUG_BREAKPOINT_GET_STATUS` | breakpoint id |
| 62 | `DEBUG_BREAKPOINT_SET_STATUS` | breakpoint id + status |
| 63 | `DEBUG_BREAKPOINT_ACTIVE` | breakpoint id |
| 64 | `DEBUG_BREAKPOINT_DISABLE` | breakpoint id |
| 65 | `DEBUG_BREAKPOINT_GET_ALL` | — |
| 66 | `DEBUG_BREAKPOINT_GET_UPDATES` | — |

### Debug: Watchpoints

| cmd | Name | Data |
|-----|------|------|
| 67 | `DEBUG_WATCHPOINT_ADD` | watchpoint definition |
| 68 | `DEBUG_WATCHPOINT_DEL_ALL` | — |
| 69 | `DEBUG_WATCHPOINT_DEL` | watchpoint id |
| 70 | `DEBUG_WATCHPOINT_GET_UPDATES` | — |
| 71 | `DEBUG_WATCHPOINT_GET_ALL` | — |

### Debug: Memory Edits

| cmd | Name | Data |
|-----|------|------|
| 72 | `DEBUG_MEMORY_EDIT_ADD` | edit definition |
| 73 | `DEBUG_MEMORY_EDIT_DEL_ALL` | — |
| 74 | `DEBUG_MEMORY_EDIT_DEL` | edit id |
| 75 | `DEBUG_MEMORY_EDIT_GET` | edit id |
| 76 | `DEBUG_MEMORY_EDIT_EXISTS` | edit id |

### Debug: Code Performance

| cmd | Name | Data |
|-----|------|------|
| 77 | `DEBUG_CODE_PERF_ADD` | perf region definition |
| 78 | `DEBUG_CODE_PERF_DEL_ALL` | — |
| 79 | `DEBUG_CODE_PERF_DEL` | perf region id |
| 80 | `DEBUG_CODE_PERF_GET` | perf region id |
| 81 | `DEBUG_CODE_PERF_EXISTS` | perf region id |

### Debug: Lua Scripts

| cmd | Name | Data |
|-----|------|------|
| 82 | `DEBUG_SCRIPT_ADD` | script definition |
| 83 | `DEBUG_SCRIPT_DEL_ALL` | — |
| 84 | `DEBUG_SCRIPT_DEL` | script id |
| 85 | `DEBUG_SCRIPT_GET_ALL` | — |
| 86 | `DEBUG_SCRIPT_GET_UPDATES` | — |

### Debug: Recorder

| cmd | Name |
|-----|------|
| 52 | `DEBUG_RECORDER_RESET` |
| 53 | `DEBUG_RECORDER_PLAY_FORWARD` |
| 54 | `DEBUG_RECORDER_PLAY_REVERSE` |
| 55 | `DEBUG_RECORDER_GET_STATE_RECORDED` |
| 56 | `DEBUG_RECORDER_GET_STATE_CURRENT` |
| 57 | `DEBUG_RECORDER_SERIALIZE` |
| 58 | `DEBUG_RECORDER_DESERIALIZE` |

### Debug: Trace Log

| cmd | Name |
|-----|------|
| 87 | `DEBUG_TRACE_LOG_ENABLE` |
| 88 | `DEBUG_TRACE_LOG_DISABLE` |

### Debug: Other

| cmd | Name | Data |
|-----|------|------|
| 50 | `DEBUG_ATTACH` | `{"data": bool}` |
| 51 | `DEBUG_RESET` | — |

## Throughput

- Frame size: 768 × 312 × 4 bytes = 958,464 bytes
- At 50 fps: ~48 MB/s
- TCP loopback throughput: ~700 MB/s
- Headroom: ~14×

The `GET_FRAME_RAW` command bypasses MessagePack serialization for frame data, sending raw pixels with a minimal 12-byte header.
