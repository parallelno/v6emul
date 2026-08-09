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
  maxSourceBytes: number;
  maxRecords: number;
  maxErrorBytes: number;
  maxInstructionsPerRun: number;
  maxExecutionMilliseconds: number;
}

interface ScriptCapabilities {
  scriptSchema: 1;
  scriptServerAllocatedIds: true;
  scriptPathSources: true;
  scriptExplicitCompile: true;
  scriptRunOnce: true;
  scriptBulkDisable: true;
  scriptMutationsWhileRunning: boolean;
  scriptRunOnceWhileRunning: boolean;
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

type ScriptRuntime =
  | { status: 'never_run'; error: null }
  | { status: 'succeeded'; error: null }
  | { status: 'error'; error: string };

interface ScriptSnapshot extends ScriptInput {
  scriptId: number;
  compilation: ScriptCompilation;
  runtime: ScriptRuntime;
}

interface ScriptIdRequest {
  scriptId: number;
}

interface ScriptMutationResponse {
  updates: number;
  script: ScriptSnapshot;
}

interface ScriptCollectionResponse {
  updates: number;
  scripts: ScriptSnapshot[];
}

interface ScriptRunOnceResponse {
  scriptId: number;
  succeeded: boolean;
  breakRequested: boolean;
  updates: number;
  runtime: ScriptRuntime;
  error?: string;
}
```

`name`, `path`, and `active` are client-writable. `active` is the user's requested scheduling state and is never rewritten as a consequence of compilation or runtime failure. `scriptId`, `compilation`, and `runtime` are server-owned.

A script is eligible for scheduled execution only when `active` is true, `compilation.status` is `compiled`, and `runtime.status` is not `error`. `RUN_ONCE` ignores `active` and may retry a script in runtime-error state, but it requires a compiled function. Compilation state always takes priority: a script without a successfully compiled function is never executed.

Paths use a platform-neutral UTF-8 wire representation with `/` separators. Windows drive paths use `C:/...` and UNC paths use `//server/share/...`; POSIX paths use `/...`. The server converts the wire path to its native `std::filesystem::path`, applies lexical normalization, and returns the normalized generic UTF-8 representation. It does not resolve symlinks or require the target to exist during normalization.

## 4. Commands

IDs `105..109` are proposed and must be confirmed before release.

| Command | ID | Request | Successful data |
|---|---:|---|---|
| `DEBUG_SCRIPT_ADD` | 84 | `ScriptInput` | `ScriptMutationResponse` |
| `DEBUG_SCRIPT_DEL_ALL` | 85 | Empty object | No data |
| `DEBUG_SCRIPT_DEL` | 86 | `{ scriptId }` | No data |
| `DEBUG_SCRIPT_GET_ALL` | 87 | Empty object | `ScriptCollectionResponse` |
| `DEBUG_SCRIPT_GET_UPDATES` | 88 | Empty object | `{ updates: uint32 }` |
| `DEBUG_SCRIPT_EDIT` | 105 | `{ scriptId, ...ScriptInput }` | `ScriptMutationResponse` |
| `DEBUG_SCRIPT_COMPILE` | 106 | `{ scriptId }` | `ScriptMutationResponse` |
| `DEBUG_SCRIPT_RUN_ONCE` | 107 | `{ scriptId }` | `ScriptRunOnceResponse` |
| `DEBUG_SCRIPT_DISABLE` | 108 | `{ scriptId }` | `ScriptMutationResponse` |
| `DEBUG_SCRIPT_DISABLE_ALL` | 109 | Empty object | `{ disabled: number }` |

### Add

- Create only; the server allocates a monotonic non-negative ID.
- Duplicate names and paths are allowed.
- Normalize the path, create the record, read the file, and compile it.
- A missing, unreadable, non-regular, or invalid Lua file creates the record in compilation-error state without changing the requested `active` value.
- Initialize runtime state to `never_run`.
- Return the resulting snapshot, including compilation failure, as a successful mutation response.

### Edit

- Replace all writable fields while preserving `scriptId`.
- Name-only and Activity-only changes preserve the compiled function and compilation state.
- A Path change reads and compiles the new file.
- A Path change resets runtime state to `never_run`.
- A failed Path compile stores the compilation error and leaves no stale runnable function. It does not change `active`.
- Setting `active: true` on a script in compilation-error or runtime-error state is valid, but does not make the script eligible for scheduled execution.

### Compile

- Re-read and compile the record's current Path.
- Success replaces the compiled function and clears the previous error without changing requested Activity.
- Failure removes any stale runnable function and stores the bounded compilation error without changing requested Activity.
- Every completed Compile resets runtime state to `never_run` because the result applies to a new compiled generation or to no compiled function.
- Return the resulting snapshot so compilation failures do not require a follow-up query.

### Run Once

- Execute the current compiled function once without reading or changing Activity.
- Execute on the emulation operation path using the current CPU, memory, I/O, and display states supplied to the debugger request handler. This is valid while stopped and, when `scriptRunOnceWhileRunning` is true, at the next serialized request boundary while running.
- Reject a script whose compilation status is not `compiled` with `invalid_request`, `details.field = "scriptId"`, and `details.reason = "not_compiled"`.
- Success stores runtime status `succeeded` and returns `succeeded: true`.
- Runtime failure, including instruction-budget exhaustion, stores the bounded runtime error and returns `succeeded: false`. It does not change `active` or compilation state.
- A successful retry replaces runtime-error state with `succeeded`, allowing an active script to resume scheduled execution.
- `Break()` completes the current Lua invocation, returns `breakRequested: true`, transitions the emulator to stopped state, and publishes a stop record with reason `script` and the triggering `scriptId`. `Break()` is not a runtime error.

### Disable

- Idempotently set one record's Activity to false.
- Preserve its compiled function and compilation state.
- Preserve its runtime state.

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

- Return `{ updates, scripts }` as one coherent snapshot taken on the emulation thread; `scripts` is always an array, including `[]`.
- Order snapshots by ascending `scriptId`.

### Get Updates

- Return a non-consuming wrapping `uint32` counter.
- Treat the counter as the collection revision and increment it once per command that changes observable collection state, regardless of how many records the command changes.
- Add always increments once. Edit increments once only when a normalized writable field changes. Compile increments once whenever a compile attempt completes because the compiled generation may have changed even when its public status did not.
- Runtime transitions among `never_run`, `succeeded`, and `error`, including a changed runtime error string, increment once. Repeated successful executions and repeated identical runtime failures do not increment.
- Activity changes and effective Delete operations increment once.
- Do not increment for rejected requests or no-op Edit, Disable, Delete, Disable All, or Delete All.

## 5. Validation and Errors

- Reject missing, extra, mistyped, invalid UTF-8, and over-limit fields.
- Require non-empty names and absolute paths in the wire format described above. Normalize paths before storage.
- Apply `maxNameBytes` to the UTF-8 name and `maxPathBytes` to the normalized generic UTF-8 path.
- Before allocating a source buffer, reject a file larger than `maxSourceBytes` into compilation-error state. Read files in binary mode, require valid UTF-8 Lua source without NUL bytes, and pass the exact byte length to Lua rather than relying on NUL termination.
- IDs are integers in `0..2147483647` and are not reused during one collection lifetime.
- Enforce `maxRecords` and detect ID exhaustion before allocation.
- Bound errors to `maxErrorBytes` without producing invalid UTF-8.
- Limit each scheduled or Run Once invocation to both `maxInstructionsPerRun` Lua VM instructions and `maxExecutionMilliseconds` elapsed wall-clock time using a Lua debug hook. Budget exhaustion is a runtime error. The hook must be removed after every invocation, including error paths.
- Do not expose unbounded blocking Lua standard-library operations such as process execution, dynamic native-library loading, or blocking file/process pipes. Every server-provided Lua callback must itself be bounded so execution cannot remain inside native code beyond the advertised deadline.
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
- Advertise independently whether mutations and Run Once are accepted while execution is running. A request accepted while running executes at the next emulation-thread request boundary.
- Records, IDs, compiled functions, statuses, and update counter survive reset, restart, ROM load, and TCP reconnect while the same debugger/Lua environment exists.
- Destroying the debugger clears the collection and permits IDs to restart in the new lifetime.
- Debug detachment suspends scheduled script execution without changing `active` or any record state. Reattachment resumes eligible scripts.
- Lua globals and other Lua-environment side effects are shared by all scripts and survive reset, restart, ROM load, reconnect, compile, disable, and delete. They are released only when the debugger/Lua environment is destroyed. Clients must not treat deleting a record as undoing arbitrary Lua side effects.
- Track the currently executing `scriptId` as the owner of each script-created UI item. Replacing an item transfers ownership to the latest writer. Remove UI items owned by a script when it becomes inactive or non-runnable, or when it is deleted. Clear all script-created UI output on reset, restart, ROM load, debug detachment, Delete All, and debugger destruction. Eligible scripts recreate their output after execution resumes.

## 7. Server Tests

Cover:

1. Capability and command advertisement.
2. Empty and ascending-ID Get All responses.
3. Monotonic IDs, duplicate names/paths, capacity, and ID exhaustion.
4. Add and Path-edit compile success, syntax failure, missing file, unreadable file, source-size limit, source UTF-8/NUL validation, and oversized error truncation.
5. Name-only and Activity-only edits preserving compiled and runtime state; compilation priority over requested Activity.
6. Explicit Compile replacing source and preventing stale execution after failure.
7. Run Once while stopped and running, runtime success/error recovery, instruction and wall-clock budget exhaustion, unavailable blocking APIs, Activity preservation, current hardware-state access, and `Break()` stop records.
8. Idempotent Disable/Delete and atomic Disable All/Delete All.
9. Exact one-per-command update-counter increments, coherent `{ updates, scripts }` snapshots, no-op behavior, and wraparound.
10. Missing/extra/wrong-type fields, invalid UTF-8, cross-platform path forms and normalization, path limits, relative paths, and unknown IDs.
11. Reset, restart, ROM load, reconnect, debug detach/reattach, debugger replacement, and running-state behavior.
12. Lua registry references are released on replacement and deletion; Lua globals and owned UI output follow the documented lifecycle.

## 8. Implementation Checklist

- [x] Confirm and reserve command IDs `105..109` in `Hardware::Req`.
- [x] Define script limits and expose all script capabilities through `GET_SERVER_INFO`.
- [x] Replace the legacy script record with server-owned ID, normalized path, requested Activity, compilation state, runtime state, and Lua registry reference.
- [x] Move ID allocation and the wrapping update revision into the `Scripts` collection; enforce capacity and ID exhaustion.
- [x] Implement strict UTF-8 request validation, exact-field validation, portable absolute-path parsing, lexical normalization, and configured byte limits.
- [x] Implement bounded binary source loading, regular-file checks, UTF-8/NUL validation, and length-aware Lua compilation.
- [x] Implement registry-reference replacement so failed recompilation and deletion cannot leave stale runnable functions or leak references.
- [x] Implement Add, Edit, Compile, Disable, Disable All, Delete, Delete All, Get All, and Get Updates with the specified no-op and revision semantics.
- [x] Return coherent mutation snapshots and atomic `{ updates, scripts }` collection snapshots ordered by `scriptId`.
- [x] Implement scheduled-execution eligibility from requested Activity, compilation state, and runtime state without rewriting Activity after failures.
- [x] Implement Run Once with current hardware state, runtime-state updates, retry behavior, and running-state capability enforcement.
- [x] Add Lua instruction and wall-clock budgets, guaranteed hook cleanup, bounded server callbacks, and removal of unbounded blocking standard-library APIs.
- [x] Implement `Break()` propagation to emulator stop state and publish a `script` stop record containing `scriptId`.
- [x] Track UI-item ownership by executing `scriptId` and implement cleanup on inactivity, failure, deletion, reset, restart, ROM load, debug detachment, and debugger destruction.
- [x] Preserve records and Lua-environment state across reset, restart, ROM load, and reconnect; suspend and resume scheduled execution across debug detach/reattach.
- [x] Translate collection, validation, unknown-ID, and not-compiled failures into the documented structured IPC errors.
- [x] Remove the legacy `{ id, active, code, comment }` request and response behavior from commands `84..88`.
- [x] Update protocol documentation and test-client helpers for schema 1 commands, capabilities, snapshots, and errors.
- [x] Add the complete server-test matrix from Section 7, including exact revision increments, resource exhaustion, cleanup, and lifecycle cases.
- [x] Run focused script/core tests, IPC tests, the full CTest suite, and leak/sanitizer checks where available.

Verification on 2026-04-08: the Release CTest suite passed 10/10 tests, the editor dashboard IPC invocation completed under the configured 30-second project timeout, and the focused IPC suite passed under MSVC AddressSanitizer.