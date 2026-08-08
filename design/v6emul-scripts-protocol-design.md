# v6emul Scripts Protocol Design

**Status:** Proposed
**Date:** 2026-08-08
**Owner:** v6emul maintainers

## 1. Scope

Define a versioned, path-based Lua script protocol for listing, adding, editing, compiling, running, disabling, and deleting scripts.

The legacy commands `84..88` use `{ id, active, code, comment }`, expose no compilation result, and do not provide stable server-assigned identity. Schema 1 replaces that public record contract. Do not keep the legacy behavior available.

## 2. Capabilities

Advertise through `GET_SERVER_INFO.capabilities`:

```ts
interface ScriptLimits {
  maxNameBytes: number;
  maxPathBytes: number;
  maxRecords: number;
  maxErrorBytes: number;
}

interface ScriptCapabilities {
  scriptSchema: 1;
  scriptServerAllocatedIds: true;
  scriptPathSources: true;
  scriptExplicitCompile: true;
  scriptRunOnce: true;
  scriptBulkDisable: true;
  scriptMutationsWhileRunning: boolean;
  scriptLimits: ScriptLimits;
}
```

Advertise every supported command in `GET_SERVER_INFO.commands`.

## 3. Models

```ts
interface ScriptInput {
  name: string;
  path: string;
  active: boolean;
}

type ScriptCompilation =
  | { status: 'compiled'; error: null }
  | { status: 'error'; error: string };

interface ScriptSnapshot extends ScriptInput {
  scriptId: number;
  compilation: ScriptCompilation;
}

interface ScriptIdRequest {
  scriptId: number;
}

interface ScriptMutationResponse {
  scriptId: number;
}

interface ScriptRunOnceResponse {
  scriptId: number;
  succeeded: boolean;
  error?: string;
}
```

`name`, `path`, and `active` are client-writable. `scriptId` and `compilation` are server-owned. Returned paths are absolute and normalized for the server platform.

## 4. Commands

IDs `105..109` are proposed and must be confirmed before release.

| Command | ID | Request | Successful data |
|---|---:|---|---|
| `DEBUG_SCRIPT_ADD` | 84 | `ScriptInput` | `{ scriptId }` |
| `DEBUG_SCRIPT_DEL_ALL` | 85 | Empty object | No data |
| `DEBUG_SCRIPT_DEL` | 86 | `{ scriptId }` | No data |
| `DEBUG_SCRIPT_GET_ALL` | 87 | Empty object | `ScriptSnapshot[]` |
| `DEBUG_SCRIPT_GET_UPDATES` | 88 | Empty object | `{ updates: uint32 }` |
| `DEBUG_SCRIPT_EDIT` | 105 | `{ scriptId, ...ScriptInput }` | `{ scriptId }` |
| `DEBUG_SCRIPT_COMPILE` | 106 | `{ scriptId }` | `{ scriptId }` |
| `DEBUG_SCRIPT_RUN_ONCE` | 107 | `{ scriptId }` | `ScriptRunOnceResponse` |
| `DEBUG_SCRIPT_DISABLE` | 108 | `{ scriptId }` | `{ scriptId }` |
| `DEBUG_SCRIPT_DISABLE_ALL` | 109 | Empty object | `{ disabled: number }` |

### Add

- Create only; the server allocates a monotonic non-negative ID.
- Duplicate names and paths are allowed.
- Normalize the path, create the record, read the file, and compile it.
- A missing, unreadable, non-regular, or invalid Lua file creates the record in compilation-error state with `active: false`.

### Edit

- Replace all writable fields while preserving `scriptId`.
- Name-only and Activity-only changes preserve the compiled function and compilation state.
- A Path change reads and compiles the new file.
- A failed Path compile stores the error, disables the record, and leaves no stale runnable function.

### Compile

- Re-read and compile the record's current Path.
- Success replaces the compiled function and clears the previous error without changing requested Activity.
- Failure removes any stale runnable function, stores the bounded error, and sets `active: false`.

### Run Once

- Execute the current compiled function once without changing Activity.
- Success returns `succeeded: true`, stores no execution metadata, and does not increment the update counter.
- Runtime failure returns `succeeded: false`, stores the bounded error, disables the script, and increments the update counter when observable state changes.
- Define and test how script `Break()` affects emulator stop state.

### Disable

- Idempotently set one record's Activity to false.
- Preserve its compiled function and compilation state.

### Disable All

- Atomically disable every active record on the emulation thread.
- Return the number of records changed. Never expose a partially updated collection.

### Delete

- Release the selected Lua registry reference and remove its record.
- Missing IDs are successful no-ops.
- Never modify or delete the source file.

### Delete All

- Release all Lua registry references and clear the collection atomically.
- Never modify or delete source files.

### Get All

- Always return an array, including `[]`.
- Order snapshots by ascending `scriptId`.

### Get Updates

- Return a non-consuming wrapping `uint32` counter.
- Increment for effective Add, Edit, Compile-result change, Activity change, runtime-error state change, and Delete.
- Do not increment for rejected requests or no-op Disable, Delete, Disable All, or Delete All.

## 5. Validation and Errors

- Reject missing, extra, mistyped, invalid UTF-8, and over-limit fields.
- Require absolute paths; normalize them before storage.
- IDs are integers in `0..2147483647` and are not reused during one collection lifetime.
- Enforce `maxRecords` and detect ID exhaustion before allocation.
- Bound errors to `maxErrorBytes` without producing invalid UTF-8.
- Unknown IDs for Edit, Compile, Run Once, and Disable are invalid requests. Delete is a no-op.

Use the normal IPC error envelope:

```json
{
  "ok": false,
  "code": "invalid_request",
  "error": "Script path is invalid",
  "details": {
    "command": 84,
    "field": "path",
    "scriptId": 12,
    "reason": "optional_machine_reason"
  }
}
```

Use `details.field = "collection"` with `reason = "capacity"` or `"id_exhausted"` for allocation failures.

File-read and Lua compilation failures are retained record state, not failed IPC envelopes. Use failed envelopes only when malformed input, unsupported operations, or internal failures prevent a coherent result.

## 6. Concurrency and Lifecycle

- Serialize file I/O, Lua compilation, registry mutation, and execution on the server-owned emulation operation path.
- Publish snapshots only after an operation reaches a coherent state.
- Advertise whether mutations and Run Once are accepted while execution is running.
- Records, IDs, compiled functions, statuses, and update counter survive reset, restart, ROM load, and TCP reconnect while the same debugger/Lua environment exists.
- Destroying the debugger clears the collection and permits IDs to restart in the new lifetime.
- Document whether reset clears script-created UI output and how active scripts behave when debug callbacks are detached.

## 7. Server Tests

Cover:

1. Capability and command advertisement.
2. Empty and ascending-ID Get All responses.
3. Monotonic IDs, duplicate names/paths, capacity, and ID exhaustion.
4. Add and Path-edit compile success, syntax failure, missing file, unreadable file, and oversized error truncation.
5. Name-only and Activity-only edits preserving compiled state.
6. Explicit Compile replacing source and preventing stale execution after failure.
7. Run Once success, runtime error, Activity preservation, and `Break()` behavior.
8. Idempotent Disable/Delete and atomic Disable All/Delete All.
9. Exact update-counter increments, no-op behavior, and wraparound.
10. Missing/extra/wrong-type fields, invalid UTF-8, path limits, relative paths, and unknown IDs.
11. Reset, restart, ROM load, reconnect, debugger replacement, and running-state behavior.
12. Lua registry references are released on replacement and deletion.