# Lua Scripts

**Scripts** are short Lua programs that the emulator runs automatically after every
retired CPU instruction while the debugger is attached. They can inspect CPU and
memory state, request an emulation stop, and publish text/rectangle overlays that a
frontend renders on top of the video output.

v6emul is a headless server, so there is no built-in script editor. Everything a
frontend needs is exposed through the `DEBUG_SCRIPT_*` IPC commands; see
[IPC Protocol → Debug: Lua Scripts](ipc-protocol.md#debug-lua-scripts) for the wire
format. This document describes the scripting *semantics* and the Lua API.

## At a glance

| Item | Value |
|------|-------|
| Language | Lua 5.1 (LuaJIT runtime, JIT compiler disabled) |
| Where scripts live | Lua source files on the **server** filesystem, absolute paths |
| Source requirements | valid UTF-8, no NUL bytes, ≤ 1 MiB |
| When scheduled scripts run | after each CPU instruction, while the debugger is attached and the CPU is running |
| Per-invocation budget | 100 000 Lua VM instructions **and** 25 ms wall clock |
| Management | `DEBUG_SCRIPT_ADD/EDIT/COMPILE/RUN_ONCE/DISABLE/DISABLE_ALL/DEL/DEL_ALL/GET_ALL/GET_UPDATES` |
| Output | retained `text` / `rect` overlays polled with `DEBUG_SCRIPT_OVERLAY_GET` (110) |
| Built-in test mode | unavailable (`--halt-exit`, `--run-frames`, `--run-cycles` create no debugger) |

## 1. Execution model

A script is a *top-level Lua chunk*. The whole chunk is executed again on every
invocation — it is not a coroutine and it keeps no per-script locals between runs:

```lua
-- runs on every instruction
local pc = GetPC()          -- local: fresh on every invocation
if pc ~= 0x0380 then return end
lastSeen = pc               -- global: persists (shared with every script)
Break()
```

Key rules:

- **Invocation point.** The debug subsystem calls the script scheduler right after an
  instruction retires and before the next one starts. At that moment `GetPC()` already
  points at the *next* instruction and `GetOpcode()` reports the instruction that *just*
  finished (including an injected `RST 7`, reported as `0xFF`).
- **`return` exits early.** The single most important idiom is an early-out guard on the
  first lines. A script that does real work on every instruction will dominate the
  emulation thread's frame time.
- **Budgets.** Each invocation is limited to 100 000 Lua VM instructions and 25 ms of
  wall-clock time, checked every 1 000 VM instructions. Exceeding either raises
  `Script execution budget exceeded`.
- **Errors are per-record state, not IPC failures.** Any Lua error marks the record's
  `runtime.status` as `error` and stores the bounded message (≤ 4 096 bytes). The
  emulator keeps running.
- **Runtime errors gate scheduled execution.** An active script whose runtime status is
  `error` is skipped by the scheduler. A successful `DEBUG_SCRIPT_RUN_ONCE` retry, or a
  `DEBUG_SCRIPT_COMPILE` (which resets runtime to `never_run`), makes it eligible again.
- **Scheduling eligibility:** `active == true` **and** `compilation.status == compiled`
  **and** `runtime.status != error`.
- **Order is unspecified.** Records are stored in an unordered map; do not rely on one
  script running before another. Overlay *rendering* order is deterministic
  (`scriptId`, then `itemId`).
- **Attachment.** `Scripts::Check()` is invoked from the debug callback, so scheduled
  scripts only run while a client has sent `DEBUG_ATTACH { "data": true }`. Records,
  compiled functions and Lua globals survive detach/attach.
- **Run Once** ignores `activity` and can execute while the CPU is stopped or running
  (it is dispatched at the next emulation-thread request boundary). It still requires a
  compiled function.
- **Read-only observer.** Scripts cannot write registers or memory. Use
  `DEBUG_MEMORY_EDIT` for patches; scripts only read, stop, and draw.

> Scheduled scripts are *not* executed in headless test mode, because
> `--halt-exit`/`--run-frames`/`--run-cycles` do not create a debugger.

## 2. Managing scripts over IPC

The minimal client flow:

1. `GET_SERVER_INFO` — require `capabilities.scriptSchema == 1`, command IDs `84..88`
   and `105..110`, and read `scriptLimits` / `scriptOverlayLimits`.
2. `DEBUG_ATTACH { "data": true }` — enable the debug callback (required for scheduled
   execution).
3. `DEBUG_SCRIPT_ADD { "name", "path", "active" }` — create a record; the server
   allocates the `scriptId`, reads the file, and compiles it.
4. Start `RUN` if needed; poll `DEBUG_SCRIPT_GET_UPDATES` and refresh with
   `DEBUG_SCRIPT_GET_ALL` only when the revision changes.
5. Poll `DEBUG_SCRIPT_OVERLAY_GET` and draw the returned items.

Example add (paths are **server-local, absolute, generic `/` form**):

```json
{"name": "fps overlay", "path": "C:/v6/scripts/fps.lua", "active": true}
```

The mutation response is `{ "updates": number, "script": ScriptSnapshot }`, where a
snapshot adds the server-owned fields:

```json
{
  "scriptId": 12,
  "name": "fps overlay",
  "path": "C:/v6/scripts/fps.lua",
  "active": true,
  "compilation": {"status": "compiled", "error": null},
  "runtime": {"status": "never_run", "error": null}
}
```

| Command | ID | Purpose / nuance |
|---------|----|------------------|
| `DEBUG_SCRIPT_ADD` | 84 | Create-only. A missing/unreadable/invalid file still creates the record, in `compilation.error` state. `name`/`path` are not unique. |
| `DEBUG_SCRIPT_DEL_ALL` | 85 | Releases every compiled function and removes all overlays. Never touches source files. |
| `DEBUG_SCRIPT_DEL` | 86 | Releases the compiled function and the script's overlays. Missing ID is a no-op. |
| `DEBUG_SCRIPT_GET_ALL` | 87 | `{updates, scripts}` snapshot, ascending `scriptId`. |
| `DEBUG_SCRIPT_GET_UPDATES` | 88 | Non-consuming wrapping revision counter. |
| `DEBUG_SCRIPT_EDIT` | 105 | Replaces every writable field. Name/activity-only edits keep the compiled function; a **path change** recompiles and resets runtime to `never_run`. |
| `DEBUG_SCRIPT_COMPILE` | 106 | Re-reads and compiles the current path, clears the previous error, resets runtime to `never_run`. |
| `DEBUG_SCRIPT_RUN_ONCE` | 107 | Executes the compiled chunk once; returns `succeeded`, `breakRequested`, `runtime`, optional `error`. |
| `DEBUG_SCRIPT_DISABLE` | 108 | Idempotently sets `active = false` and removes the script's overlays; keeps the compiled function. |
| `DEBUG_SCRIPT_DISABLE_ALL` | 109 | Returns `{ "disabled": n }`. |
| `DEBUG_SCRIPT_OVERLAY_GET` | 110 | Returns only changed overlays and consumes their changed flags. |

Important workflow nuance: **editing the Lua file on disk does nothing by itself.** If
the path did not change, no command re-reads the file — send `DEBUG_SCRIPT_COMPILE`.
`RUN_ONCE` also never re-reads the file.

## 3. Lua environment

The state is created with `luaL_openlibs` (so `string`, `table`, `math`, `coroutine`,
`bit`, `debug`, `jit` and — when built in — `ffi` are present), then:

- the LuaJIT JIT compiler is switched off (`LUAJIT_MODE_OFF`); scripts are always
  interpreted,
- these globals are removed to prevent accidental blocking I/O and process execution:

```text
io os package require loadfile dofile   -- all nil inside a script
```

Consequences worth knowing:

- `print(...)` still works and writes to the **server process stdout** (useful for
  `RUN_ONCE` diagnostics, dangerous inside per-instruction scripts).
- There is no `require`/`dofile`: a script is a single self-contained file. Share code by
  defining functions in globals (see below) or by concatenating source.
- String-based `load`/`loadstring` remain available; only file/process entry points were
  removed.
- **This is not a security sandbox.** `debug`, `jit` and (if enabled in the build) `ffi`
  are still loaded, so only run scripts you trust. In particular do not call
  `debug.sethook` — it replaces the hook that enforces the execution budget.
- **One Lua state for all scripts.** Globals, functions and upvalues are shared by every
  script and survive reset, restart, ROM load, compile, disable, delete and client
  reconnect. They are freed only when the process (and its debugger) is destroyed.
  Namespace your globals, e.g. `fps_lastCC`, and remember that deleting a record does not
  undo its side effects.

## 4. Intrinsic reference

All intrinsics are plain globals, case-sensitive, and read the live emulation state at
call time. They are C closures, so treat each call as relatively expensive and cache the
result in a local if you need it more than once.

### 4.1 CPU state

| Function | Returns | Meaning |
|----------|---------|---------|
| `GetCC()` | integer (64-bit) | CPU clock cycles (T-states) since reset; monotonic. |
| `GetPC()` | integer 0–65535 | Program counter, i.e. the address of the **next** instruction. |
| `GetSP()` | integer 0–65535 | Stack pointer. |
| `GetPSW()` | integer 0–65535 | `A << 8` OR `F` (the 8080 PSW word). |
| `GetBC()`, `GetDE()`, `GetHL()` | integer 0–65535 | Register pairs, `high << 8` OR `low`. |
| `GetA()` | integer 0–255 | Accumulator. |
| `GetF()` | integer 0–255 | Flags byte (layout below). |
| `GetB()`, `GetC()`, `GetD()`, `GetE()`, `GetH()`, `GetL()` | integer 0–255 | Individual registers. |
| `GetFlagS()` | boolean | Sign. |
| `GetFlagZ()` | boolean | Zero. |
| `GetFlagAC()` | boolean | Auxiliary (half) carry. |
| `GetFlagP()` | boolean | Parity. |
| `GetFlagC()` | boolean | Carry. |
| `GetINTE()` | boolean | Interrupts enabled (`EI`). |
| `GetIFF()` | boolean | 50 Hz interrupt flip-flop pending until the next `RST 7`. |
| `GetHLTA()` | boolean | CPU is halted (`HLT`). |
| `GetMachineCycles()` | integer 0–5 | Machine-cycle index of the in-flight instruction. Scripts run between instructions, so this is normally `0`; it is **not** a cumulative counter. |

`GetF()` bit layout (bits 1, 3 and 5 are constants on the Vector-06C):

```
bit  7 6 5 4 3 2 1 0
     S Z - AC - P 1 C
```

Flag helpers return Lua booleans, so they compose naturally:

```lua
if GetFlagZ() and not GetFlagC() then
    -- Z set, C clear
end
```

Do not do arithmetic on the boolean getters; use `bit.*` when you need to mask `GetF()`.

### 4.2 Memory

| Function | Returns | Meaning |
|----------|---------|---------|
| `GetByteGlobal(addr)` | integer 0–255 | Byte at a **global** address in the 64 KB main RAM array. |

`GetByteGlobal` is a raw main-RAM read:

- it does **not** translate CPU addresses through RAM-disk mappings,
- it does **not** see the ROM overlay — with the ROM enabled the CPU may execute bytes
  that differ from what this function returns,
- it always returns the byte *after* the instruction that just retired has written it.

The address is a plain integer index with no validation at the Lua layer; keep it in
`0..65535` and avoid expressions that can produce `nil`, negative or NaN values.

There is no word/DMA helper; compose it:

```lua
local function GetWord(addr)
    return GetByteGlobal(addr) + GetByteGlobal(addr + 1) * 256
end
```

### 4.3 Labels

| Function | Returns | Meaning |
|----------|---------|---------|
| `GetLabelAddr(name)` | integer | Address of `name`, or `-1` when the label is unknown. |

Labels come from the debugger's label table (the same store the disassembler uses). The
current v6emul IPC surface has no command that populates it, so in this build
`GetLabelAddr` normally returns `-1`; treat it as forward-looking. Always handle the
failure case:

```lua
local start = GetLabelAddr("MAIN_LOOP")
if start < 0 then return end
if GetPC() ~= start then return end
```

### 4.4 Execution control

| Function | Meaning |
|----------|---------|
| `Break()` | Request an emulation stop. Takes no arguments and returns nothing. |

`Break()` does **not** abort the running Lua chunk: the chunk finishes normally (subject
to the budget), the script's runtime status becomes `succeeded`, and the emulation stops
after the current instruction. v6emul then publishes a stop record with
`reason = "script"` and the triggering `scriptId` (read it with `GET_STOP_RECORD`).
Other active scripts still run for that instruction. `RUN_ONCE` follows the same rules —
it returns `breakRequested: true` and stops the CPU — so a single diagnostic run can
pause a running emulator.

### 4.5 Overlays

```text
DrawText(id, text, x, y, <color = 0xFFFFFFFF>, <vectorScreenCoords = true>)
DrawRect(id, x, y, width, height, <filled = false>, <color = 0xFFFFFFFF>, <vectorScreenCoords = true>)
```

| Parameter | Type | Rules |
|-----------|------|-------|
| `id` | integer | `0 .. 2147483647`, scoped to the executing script. Reusing an id replaces the overlay, including its type (text ↔ rect). |
| `text` | string | Valid UTF-8, no NUL bytes, ≤ 4 096 encoded bytes. |
| `x`, `y` | number | Finite, `abs(value) ≤ 1 000 000`. Negative values are measured from the right/bottom edge. |
| `width`, `height` | number | Finite, `0 ≤ value ≤ 1 000 000`. |
| `filled` | boolean | `false` (default) = outline, `true` = filled. |
| `color` | integer | Unsigned `0xRRGGBBAA` in `0 .. 4294967295`. Omitted = opaque white. |
| `vectorScreenCoords` | boolean | `true` (default) = 512×256 Vector-06C active area; `false` = full 768×312 framebuffer. Omitted = `true`. |

Validation is **strict** — there is no implicit coercion:

- `id` and `color` must be numbers with no fractional part (`"1"`, `1.5` and `-1` are
  rejected), and `color` must fit in 32 bits unsigned;
- `x`, `y`, `width`, `height` must be numbers (NaN and infinity are rejected);
- `text` must be a string; `filled` and `vectorScreenCoords` must be booleans;
- `DrawText` takes 4–6 arguments, `DrawRect` takes 5–8; optional arguments are
  positional and cannot be skipped.

Any violation raises a **script runtime error** for that invocation: the Lua chunk stops,
the record's runtime status becomes `error`, and no further scheduled runs of that script
happen until it is fixed with `RUN_ONCE`/`COMPILE`.

`DrawRectFilled` from Devector does not exist here: v6emul uses one rectangle primitive
with a `filled` flag (positional order differs from Devector as well).

## 5. Overlay model

Overlays are *retained server state*, not pixels in the emulated framebuffer. The server
stores the latest draw for each `(scriptId, id)` pair and lets the frontend render it.

- **Identity.** `scriptId` is the record that executed the draw; `id` is the Lua-supplied
  item id. Two scripts may both use `id = 1`.
- **One item per id.** Drawing an existing id replaces the previous overlay (any type).
  There is no "delete" intrinsic — hide an item by drawing it with `alpha = 0`, e.g.
  `DrawText(1, "", 0, 0, 0x00000000)`. Hidden items remain retained and still count toward
  the limits.
- **Changed-only delivery.** `DEBUG_SCRIPT_OVERLAY_GET` (110) returns only items whose
  content changed since the last poll, sorted by `scriptId` then `id`, and *consumes* the
  changed flag. Calling `DrawText`/`DrawRect` again with identical values does not
  produce an update.
- **First poll after connect** returns every retained overlay, because a new client
  session re-marks all items as changed.
- **Coordinates are client-side.** The server passes `x`, `y`, `width`, `height`,
  `vectorScreenCoords` and `color` through unchanged. Positive `x`/`y` are measured from
  the left/top, negative values from the right/bottom; the drawing order is the response
  order.
- **Partial results survive failure.** Items drawn before an error (including a budget
  error) stay stored; the invocation is not rolled back.
- **Lifecycle.**

| Event | Overlay effect |
|-------|----------------|
| Successful or failed invocation | Items drawn before the failure stay stored |
| Path edit or `COMPILE` | Kept as-is until the script redraws them |
| `EDIT` with `active: false`, `DISABLE`, `DISABLE_ALL` | Removed for that script |
| `DEL`, `DEL_ALL` | Removed |
| Reset, restart, ROM load, stop/start, detach, TCP reconnect | Kept (reconnect re-publishes all) |
| Debugger/process destruction | Discarded |

## 6. Limits and validation

Advertised through `GET_SERVER_INFO` and enforced by the server:

| Limit | Value |
|-------|-------|
| `maxNameBytes` | 1 024 |
| `maxPathBytes` | 4 096 |
| `maxSourceBytes` | 1 048 576 (1 MiB) |
| `maxRecords` | 256 |
| `maxErrorBytes` | 4 096 |
| `maxInstructionsPerRun` | 100 000 |
| `maxExecutionMilliseconds` | 25 |
| `maxItemsPerScript` | 256 |
| `maxItemsTotal` | 1 024 |
| `maxTextBytes` | 4 096 |
| `maxCoordinateMagnitude` | 1 000 000 |

Record input rules:

- `name` — non-empty UTF-8, ≤ `maxNameBytes`. Duplicates are allowed.
- `path` — absolute, generic UTF-8 path with `/` separators: `C:/...` or
  `//server/share/...` on Windows, `/...` on POSIX. Lexically normalized by the server
  without resolving symlinks; the file does not have to exist.
- `active` — boolean; the requested scheduling state. It is never rewritten because of a
  compilation or runtime failure.
- The file is read in binary mode; it must be a regular file with valid UTF-8 Lua source
  containing no NUL bytes, within `maxSourceBytes`.

A failed compile is *record state*, not a failed IPC request: `ADD`/`EDIT`/`COMPILE` still
return a successful mutation with `compilation.status = "error"` and the bounded message.
Compilation always takes priority over `active`: a script that is not compiled is never
executed, not even by `RUN_ONCE`.

## 7. Examples

### 7.1 Stop when execution reaches an address

```lua
-- break-at-0380.lua
-- Stops the emulator whenever the CPU is about to execute 0x0380.
-- Why GetPC() and not "the current instruction": scripts run after the
-- instruction retired, so PC already points at the next one.
if GetPC() ~= 0x0380 then return end
Break()
```

If you continue after the stop, the script stops again on the next visit. Add a global
flag if you want a true one-shot:

```lua
if hit0380 then return end
if GetPC() ~= 0x0380 then return end
hit0380 = true
Break()
```

### 7.2 Log the first `HLT` and stop

```lua
-- first-hlt.lua
-- Reports the first time a HLT instruction retires, then stops the CPU.
if sawHLT then return end
if GetOpcode() ~= 0x76 then return end

sawHLT = true
print(string.format("HLT executed, PC=%04X SP=%04X HLTA=%s",
    GetPC(), GetSP(), tostring(GetHLTA())))
Break()
```

(`GetOpcode()` reports the instruction that just finished, so `GetPC()` has already moved
on — by the instruction length, or to a jump/call/return target. Do not assume
`GetPC() - 1` is the opcode's address.)

### 7.3 Guarded stack/interrupt check (Devector-style)

```lua
-- bc-risk.lua
-- Warn if BC may be clobbered inside a critical stack section.
local sp = GetSP()
if sp <= 0x7FBE or sp >= 0x7FFE then return end

local opcode = GetOpcode()
if opcode > 0x4F then return end
if opcode > 0x0E and opcode < 0x40 then return end

-- build the lookup table once, not on every visit
local skip = bcRiskSkip
if not skip then
    skip = {
        [0x00] = true, [0x02] = true, [0x07] = true,
        [0x08] = true, [0x09] = true, [0x0A] = true,
    }
    bcRiskSkip = skip
end
if skip[opcode] then return end
if not GetINTE() then return end

-- Interrupt vector must point at 0x0578 (C3 78 05)
if GetByteGlobal(0x38) ~= 0xC3 or
   GetByteGlobal(0x39) ~= 0x78 or
   GetByteGlobal(0x3A) ~= 0x05 then
    return
end

print(string.format("Warning: BC overwrite risk at PC=%04X opcode=%02X",
    GetPC(), opcode))
Break()
```

`GetByteGlobal` reads main RAM only. If the ROM overlay is enabled, the bytes the CPU
actually executes at `0x0038` come from ROM, so this check is only meaningful while RAM
is mapped there.

### 7.4 FPS counter overlay (throttled by cycle count)

```lua
-- fps.lua
-- Draws the 16-bit counter maintained by the program at 0x05CC/0x05CD.
local cc = GetCC()
if cc - (fps_lastCC or 0) < 100000 then return end   -- ~30 updates/second
fps_lastCC = cc

local fps = GetByteGlobal(0x05CC) + GetByteGlobal(0x05CD) * 256
DrawText(1, string.format("FPS %d", fps), 8, 8, 0xFFFFFFFF, true)
```

Because identical draws are not re-published, the frontend only sees a change when the
text actually changes.

### 7.5 HUD with text + bar, hidden instead of deleted

```lua
-- hud.lua
local hp = GetByteGlobal(0x0500)          -- 0..255
local on = GetByteGlobal(0x0501) ~= 0

-- item 1: label (hidden as fully transparent text when the HUD is off)
DrawText(1, "HP", 8, -20,
    on and 0xFFFFFFFF or 0x00000000, true)

-- item 2: background and 3: bar (both hidden by zero alpha)
DrawRect(2, 24, -20, 108, 10, true,
    on and 0x202020FF or 0x00000000, true)
DrawRect(3, 25, -19, hp, 8, true,
    on and 0x00FF00FF or 0x00000000, true)
```

Overlays cannot be removed by a script, so "off" means alpha `0`; the items stay retained
and count toward `maxItemsPerScript`/`maxItemsTotal`.

### 7.6 Detect a timer interrupt (`RST 7`)

```lua
-- tick.lua
-- The 50 Hz interrupt is injected as opcode 0xFF without a memory fetch.
if GetOpcode() ~= 0xFF then return end

ticks = (ticks or 0) + 1
if ticks % 50 == 0 then
    DrawText(4, string.format("t=%d", math.floor(ticks / 50)), 8, 24, 0xFFFF00FF, true)
end
```

`GetIFF()` tells you that a 50 Hz interrupt is pending, `GetINTE()` whether interrupts are
enabled, and `GetHLTA()` whether the CPU is halted.

### 7.7 Watch a memory location change

```lua
-- watch-5000.lua
-- Prints the new value and the PC right after 0x5000 changes.
local value = GetByteGlobal(0x5000)
if value == watched5000 then return end      -- first run initializes the global

local previous = watched5000
watched5000 = value
if previous == nil then return end

print(string.format("0x5000: %02X -> %02X near PC=%04X",
    previous, value, GetPC()))
```

### 7.8 Run Once diagnostics

`RUN_ONCE` ignores the activity flag and is handled by the debugger request path rather
than the debug callback, so a small "dump state" script can be executed on demand — even
without `DEBUG_ATTACH` — while the emulator is stopped or running.

```lua
-- dump.lua
local function hex(v, digits)
    return string.format("%0" .. digits .. "X", v)
end

print("PC=" .. hex(GetPC(), 4) ..
      " SP=" .. hex(GetSP(), 4) ..
      " AF=" .. hex(GetPSW(), 4) ..
      " BC=" .. hex(GetBC(), 4) ..
      " DE=" .. hex(GetDE(), 4) ..
      " HL=" .. hex(GetHL(), 4))
print("opcode=" .. hex(GetOpcode(), 2) ..
      " cc=" .. tostring(GetCC()) ..
      " mc=" .. tostring(GetMachineCycles()) ..
      " INTE=" .. tostring(GetINTE()) ..
      " IFF=" .. tostring(GetIFF()) ..
      " HLTA=" .. tostring(GetHLTA()))
```

Output goes to the emulator's stdout, not into the IPC response.

### 7.9 Sharing helpers between scripts

Because all scripts share one Lua state, a helper defined once is visible everywhere
afterwards. Define helpers from a script that is *not* scheduled: add the file with
`active: false` and execute it with `RUN_ONCE`, otherwise the chunk would allocate new
closures on every instruction.

```lua
-- lib.lua (add with active: false and use RUN_ONCE once)
function V6_hex16(v) return string.format("%04X", v) end
function V6_word(addr)
    return GetByteGlobal(addr) + GetByteGlobal(addr + 1) * 256
end
```

```lua
-- caller.lua
local w = V6_word(0x5C00)
DrawText(1, "W=" .. V6_hex16(w), 8, 8)
```

Deleting `lib.lua` does not remove the functions from the shared state, and another
script that happens to use the same global names will collide. Prefix your globals.

## 8. Performance checklist

Scripts run up to ~3 million times per emulated second (≈ 60 000 instructions per frame,
50 frames per second), so the scheduler path must stay trivially cheap:

1. Put the cheapest possible guard on line 1 (`GetPC()` or `GetCC()`) and `return`
   immediately when the script has nothing to do.
2. Never call an intrinsic more than once for the same value — assign it to a local.
3. Never call `print`, `string.format`, `DrawText` or `DrawRect` unconditionally; gate
   them on a trigger (address, cycle delta, or changed value).
4. Avoid creating tables/garbage on every visit. Allocate lookup tables behind a
   `if not cache then ... end` guard.
5. Prefer a cycle-based throttle (`GetCC()` delta) over per-instruction redraws for HUD
   elements.
6. Keep the whole chunk well under the 100 000-instruction / 25 ms budget; a budget error
   disables the script until you re-run or recompile it.
7. Remember that `Break()` stops the CPU after the instruction; the rest of the chunk and
   the remaining active scripts still execute.

## 9. Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| Script never runs although `active` is true | Debugger not attached (`DEBUG_ATTACH`), compilation error, or runtime status stuck at `error`. Check `compilation`/`runtime` in `GET_ALL`. |
| `compilation.status = "error"` after editing the file | The file is not re-read automatically — send `DEBUG_SCRIPT_COMPILE`. |
| `DrawText` fails with "must be a number"/"must be an integer" | Strict typing: numeric strings and fractional values are rejected. Use `tonumber` and integer values. |
| Overlay never appears in the frontend | The client polled before the draw and did not poll again — `OVERLAY_GET` is consuming. Or you redrew identical values (no update is published). |
| Overlay stays on screen after the script stops drawing | Overlays are retained. Hide it with alpha `0` or disable/delete the script. |
| "Overlay limit per script exceeded" | More than 256 ids, or more than 1 024 items across all scripts. Reuse ids and hide instead of adding new ones. |
| "Script execution budget exceeded" | Infinite loop or heavy work per instruction. Add an early-out guard and a cycle throttle. |
| Emulation slows down after adding scripts | Expected cost of running Lua on every instruction; reduce the number of active scripts and their per-visit work. |
| `GetLabelAddr` always returns `-1` | The label table is not populated by the current IPC surface; handle `-1` in scripts. |

### Differences from Devector's script window

The Devector `README.md` describes the same intrinsics, but v6emul differs in a few
places:

| Devector | v6emul |
|----------|--------|
| Script is edited inside the app (Debug Data → Scripts tab) | Scripts are server-side files managed through `DEBUG_SCRIPT_*` IPC commands |
| `DrawRect(id, x, y, w, h, color?, vectorScreenCoords?)` + `DrawRectFilled(...)` | One `DrawRect(id, x, y, w, h, filled?, color?, vectorScreenCoords?)` |
| Overlays rendered by the emulator window | Overlays are returned over IPC; the frontend renders them |
| `GetLabelAddr` backed by loaded symbols | Implemented, but no IPC command populates labels yet |
| Scripts run whenever the debug window is open | Scripts run only while `DEBUG_ATTACH` is active; headless test mode never runs them |

## Related documentation

- [IPC Protocol](ipc-protocol.md#debug-lua-scripts) — commands, record schema, overlay wire format
- [Architecture](architecture.md#lua-scripting) — where `Scripts` sits in the emulator
- [CLI Reference](cli.md) — server mode and test-mode flags
- [Test Client](test-client.md) — the bundled client does not implement script commands

