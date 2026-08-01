# v6emul Unified Stop Record Design

**Status:** Proposed
**Date:** 2026-08-01
**Consumer:** v6vscode

## 1. Goal

Allow external clients to determine why emulation most recently stopped and what triggered the stop.

Today v6vscode can detect a transition from running to stopped through `IS_RUNNING`, but it must guess the reason from pending client requests and the current PC. This cannot reliably distinguish breakpoints, watchpoints, manual stops, `HLT`, or exceptions.

## 2. Protocol Addition

Add a request named `GET_STOP_RECORD`. The numeric command ID is assigned by v6emul and advertised by `GET_SERVER_INFO`.

Example response:

```json
{
  "sequence": 42,
  "reason": "watchpoint",
  "pc": 4660,
  "globalInstructionAddress": 4660,
  "watchpointIds": [7],
  "access": "write",
  "accessedGlobalAddress": 65536,
  "oldValue": 16,
  "newValue": 32,
  "description": "Watchpoint 7 matched a write"
}
```

Required fields:

- `sequence`: monotonic stop-event sequence number.
- `reason`: `pause`, `breakpoint`, `watchpoint`, `step`, `next`, `frameStep`, `halt`, `reset`, `exception`, or `unknown`.
- `pc`: CPU program counter at the stop.
- `globalInstructionAddress`: global address of the stopped instruction.

Optional trigger fields:

- `breakpointIds` and `breakpointAddress`.
- `watchpointIds`, `access`, and `accessedGlobalAddress`.
- `observedValue`, `oldValue`, and `newValue` when available.
- `exceptionCode`.
- `description` for display to the user.

ID arrays allow the response to represent overlapping breakpoints or watchpoints. Fields that do not apply to the stop reason are omitted.

## 3. Required Behavior

All emulator stop sources write through one stop-record abstraction so the reported reason and trigger describe the same event.

The record must be:

- **Non-consuming:** reading it does not clear or alter it.
- **Persistent:** it remains available until a newer stop replaces it.
- **Atomic:** clients never observe fields combined from different stop events.
- **Ordered:** `sequence` increases for every new stop, independently of breakpoint and watchpoint update counters.

v6emul must define the initial response before any stop has occurred and the sequence behavior across reset, restart, ROM load, attach, and reconnect.

Process exit and connection loss are reported by the client's process or transport lifecycle because the server may no longer be available to answer this request.

## 4. Capability Discovery

`GET_SERVER_INFO` should advertise:

```json
{
  "capabilities": {
    "stopRecordSchema": 1
  }
}
```

The `commands` list must also contain the assigned `GET_STOP_RECORD` command ID. Clients that do not see both declarations continue using legacy running-state detection and must not claim authoritative stop attribution.

## 5. v6vscode Integration

While execution is running, v6vscode polls for a new stop sequence. When the sequence changes, it maps the record to one DAP `stopped` event:

- `pause` and `halt` to the corresponding user-facing stop reason.
- `breakpoint` with `hitBreakpointIds`.
- `watchpoint` to a DAP data-breakpoint stop with the triggering watchpoint details.
- step variants to `step`.
- `exception` to `exception` with its description.
- unrecognized values to `unknown` without guessing.

Adapter-generated `stopOnEntry` may remain local because it is intentionally controlled by the extension before normal execution starts.

## 6. Acceptance Criteria

1. Repeated reads return the same record and sequence until another stop occurs.
2. Manual stop, breakpoint, watchpoint, single-step, `HLT`, reset, and exception paths produce the correct reason.
3. Breakpoint and watchpoint stops include stable trigger IDs.
4. Watchpoint stops include the matching access type and global address.
5. Two consecutive stops produce increasing sequence numbers.
6. Malformed requests return a structured error without affecting emulation.
7. `GET_SERVER_INFO` identifies support for stop-record schema 1.
