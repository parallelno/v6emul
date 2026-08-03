# v6emul Code Performance Protocol Design

**Status:** Proposed
**Date:** 2026-08-02
**Consumer:** v6vscode and other IPC clients

## 1. Scope

This document defines the proposed schema-1 IPC contract for code-performance records. It covers record creation, identity, retrieval, validation, sampling behavior, runtime lifecycle, limits, and capability discovery. JSON examples below show the successful response `data` payload; the normal IPC response envelope still applies.

The existing commands are:

| ID | Command |
|---:|---|
| 79 | `DEBUG_CODE_PERF_ADD` |
| 80 | `DEBUG_CODE_PERF_DEL_ALL` |
| 81 | `DEBUG_CODE_PERF_DEL` |
| 82 | `DEBUG_CODE_PERF_GET` |
| 83 | `DEBUG_CODE_PERF_EXISTS` |

This design adds:

| ID | Command |
|---:|---|
| 101 | `DEBUG_CODE_PERF_GET_ALL` |
| 102 | `DEBUG_CODE_PERF_EDIT` |

## 2. Current Problems

The current CodePerf interface is not a complete client contract:

- `DEBUG_CODE_PERF_GET` omits the record ID and measured statistics.
- Addresses are returned as formatted hexadecimal strings instead of numeric wire values.
- There is no operation for discovering the complete collection.
- The legacy address-keyed `DEBUG_CODE_PERF_ADD` behavior conflated creation and replacement. The interim ID-keyed behavior creates records but provides no identity-preserving edit operation.
- `GET_SERVER_INFO` does not advertise a CodePerf schema, limits, command support, or mutation behavior while running.
- Handlers index JSON fields directly and rely on implicit conversion instead of strict request validation.
- Name length, UTF-8 validity, address bounds, endpoint ordering, and maximum collection size are undefined.
- Reset and restart preserve an in-progress sample even though the CPU clock is reset, which can corrupt the next completed sample.
- The test count saturates at 20,000 while the average continues changing with a fixed weight. This is surprising unless explicitly defined.
- Equal endpoints cannot complete a sample because sampling checks the start address before the end address using `if`/`else if`.

## 3. Wire Types

Client-writable input and server-owned snapshots are separate shapes.

### CodePerfInput

```json
{
  "name": "render",
  "addrStart": 4096,
  "addrEnd": 4352,
  "active": true
}
```

### CodePerfSnapshot

```json
{
  "id": 12,
  "name": "render",
  "addrStart": 4096,
  "addrEnd": 4352,
  "active": true,
  "averageClockCycles": 37.5,
  "testCount": 128
}
```

All addresses and statistics are numeric on the wire. `id`, addresses, and `testCount` are JSON integers; `averageClockCycles` is a finite JSON number. Clients are responsible for hexadecimal address formatting, clock-cycle rounding, and localized display text.

The wire protocol uses `name` consistently. The existing internal `label` member may remain an implementation detail during migration.

## 4. Command Contract

### DEBUG_CODE_PERF_ADD

Request: `CodePerfInput` without an `id` or statistics.

ADD is create-only. The server always creates a distinct record, allocates a monotonically increasing ID, initializes its statistics to zero, and returns the complete created `CodePerfSnapshot`:

```json
{
  "id": 12,
  "name": "render",
  "addrStart": 4096,
  "addrEnd": 4352,
  "active": true,
  "averageClockCycles": 0.0,
  "testCount": 0
}
```

ADD never searches, replaces, merges, or updates an existing record. Duplicate names, start addresses, and complete endpoint pairs are allowed. Clients must use EDIT to change an existing ID.

Deleting records does not make their IDs available for reuse during the collection lifetime.

### DEBUG_CODE_PERF_EDIT

Request: `CodePerfInput` with the server-assigned `id`:

```json
{
  "id": 12,
  "name": "render frame",
  "addrStart": 4096,
  "addrEnd": 4608,
  "active": true
}
```

The request is a complete replacement of the writable configuration, not a partial patch. The operation updates the existing record without changing its ID and returns the updated `CodePerfSnapshot`.

- Changing either endpoint resets `testCount` and `averageClockCycles` to zero and cancels any in-progress sample.
- Changing only `name` preserves completed statistics and any in-progress sample.
- Changing `active` from true to false preserves completed statistics but cancels any in-progress sample.
- Changing `active` from false to true preserves completed statistics and starts with no sample in progress.
- If an edit changes both an endpoint and activity, the endpoint rule takes precedence and completed statistics reset.
- Editing a missing ID returns structured `invalid_request` with `details.command = 102` and `details.field = "id"`.

### DEBUG_CODE_PERF_GET

Request:

```json
{ "id": 12 }
```

Response: the corresponding `CodePerfSnapshot`, or JSON `null` when the ID does not exist.

### DEBUG_CODE_PERF_GET_ALL

Request: no data.

Response: a direct array of `CodePerfSnapshot` values ordered by ascending ID. An empty collection returns `[]`.

### DEBUG_CODE_PERF_EXISTS

Request:

```json
{ "id": 12 }
```

Response:

```json
{ "exists": true }
```

### DEBUG_CODE_PERF_DEL

Request:

```json
{ "id": 12 }
```

The operation removes only the selected record. A missing ID is a successful no-op.

### DEBUG_CODE_PERF_DEL_ALL

Request: no data.

The operation removes all records. Calling it on an empty collection is a successful no-op. The next-ID counter is not reset during the current collection lifetime.

## 5. Validation

Structural validation occurs in the IPC layer before dispatch. State-dependent checks, such as record existence and collection capacity, occur in the owning collection during dispatch. Both paths return structured errors through the server error boundary.

Invalid input returns `invalid_request` with at least:

```json
{
  "code": "invalid_request",
  "details": {
    "command": 79,
    "field": "addrEnd"
  }
}
```

Requirements:

- Reject missing and unknown fields.
- `name` must be a valid UTF-8 string no longer than `codePerfLimits.maxNameBytes` encoded bytes. An empty name is allowed.
- `addrStart` and `addrEnd` must be integers in `0..65535`.
- Require `addrStart < addrEnd`; wrapped ranges, equal endpoints, and reversed endpoints are rejected with `details.field = "addrEnd"`.
- `active` must be a Boolean.
- IDs must be non-negative integers representable by `dev::Id`.
- ADD accepts exactly the CodePerf input fields and rejects `id`, `averageClockCycles`, and `testCount` as unknown server-owned fields.
- EDIT requires exactly the CodePerf input fields plus `id` and applies the same field validation as ADD.
- GET, EXISTS, and DEL accept exactly one `id` field.
- GET_ALL and DEL_ALL accept an empty data object and reject all supplied fields.
- Clients cannot supply `averageClockCycles` or `testCount`.
- The server must emit a finite numeric `averageClockCycles` value.
- ADD fails with `invalid_request`, `details.command = 79`, and `details.field = "collection"` when creation is unavailable. `details.reason = "capacity"` means deletion can free record capacity; `details.reason = "id_exhausted"` means no further ID can be allocated during this collection lifetime.

The existence semantics for a well-formed ID are command-specific:

- GET returns JSON `null` for a missing ID.
- EXISTS returns `{ "exists": false }` for a missing ID.
- DEL treats a missing ID as a successful no-op.
- EDIT rejects a missing ID with `invalid_request` and `details.field = "id"`.

These missing-record results are distinct from malformed, negative, fractional, or overflowing ID values, which are rejected during structural validation.

## 6. Capability Discovery

`GET_SERVER_INFO.commands` includes all supported CodePerf command IDs, including `DEBUG_CODE_PERF_GET_ALL` and `DEBUG_CODE_PERF_EDIT`.

`GET_SERVER_INFO.capabilities` advertises:

```json
{
  "codePerfSchema": 1,
  "codePerfServerAllocatedIds": true,
  "codePerfEdit": true,
  "codePerfMutationsWhileRunning": true,
  "codePerfLimits": {
    "addressExclusive": 65536,
    "maxNameBytes": 1024,
    "maxRecords": 256,
    "maxTestCount": 20000
  }
}
```

`maxRecords` is the maximum number of simultaneously live records; deleting records frees capacity but does not make their IDs reusable. The limits are enforced by the server, not merely advertised. A conservative record limit is necessary because the current sampling algorithm examines every active record for each executed address.

When `codePerfMutationsWhileRunning` is true, ADD, EDIT, DEL, and DEL_ALL are accepted while execution is running. Requests are serialized on the emulation thread with instruction sampling, so each mutation and snapshot observes one coherent collection state.

## 7. Sampling Semantics

- A sample starts when execution reaches `addrStart` and completes when execution subsequently reaches `addrEnd`.
- Reaching `addrStart` again before `addrEnd` restarts the in-progress sample from the newer clock value.
- Inactive records do not start, complete, or update samples.
- `testCount` counts completed samples only.
- `averageClockCycles` is the running arithmetic mean of completed sample durations.
- The completion that raises `testCount` to `maxTestCount` contributes to the average and clears the in-progress sample. After that completion, the record stops sampling: no new sample starts, and `testCount` and `averageClockCycles` cease to change.
- Equal endpoints are rejected during request validation. The current start-first `if`/`else if` algorithm would otherwise never complete such a sample.
- Sampling statistics are runtime state and are not accepted from clients.
- In-progress state is represented explicitly rather than by using clock value zero as a sentinel, because a valid sample may start at cycle zero.

## 8. Lifecycle and Reset Behavior

- Records and completed statistics survive CPU reset, restart, ROM loading, and TCP reconnect while the same `Debugger` instance remains alive.
- Reset, restart, and ROM loading cancel every in-progress sample by clearing its start-clock marker. Completed `testCount` and `averageClockCycles` values remain unchanged.
- Pausing and resuming execution without a CPU clock reset preserves an in-progress sample. Time spent paused does not add emulated clock cycles, so it does not affect the completed duration.
- Adding a record initializes `testCount` and `averageClockCycles` to zero and leaves no sample in progress.
- Editing endpoints resets completed statistics; other edits preserve them according to the EDIT contract.
- Deleting a record discards its configuration, completed statistics, and in-progress sample.
- CodePerf records are runtime-only and are not loaded from or saved to debug-data files.
- Destroying the owning `Debugger` instance, including during process exit, ends the collection lifetime and discards all records, IDs, statistics, and in-progress samples.

IDs are stable only during this in-memory collection lifetime. A new `Debugger` instance starts a new collection and may allocate the same numeric IDs again.

## 9. Test Coverage

Server tests should cover:

- GET returns every snapshot field with numeric addresses and statistics.
- GET_ALL returns `[]` for an empty collection and ascending-ID snapshots otherwise.
- Multiple records may share the same start address.
- ADD always creates a distinct record and returns its complete zero-statistics snapshot.
- ADD permits duplicate names and endpoint pairs and rejects client-supplied IDs or statistics.
- IDs are monotonic and not reused after deletion or DEL_ALL.
- Collection capacity is restored by deletion, while newly allocated IDs remain monotonic.
- ID exhaustion is rejected without signed overflow or ID reuse.
- Capacity and ID-exhaustion failures have distinct `details.reason` values.
- EDIT preserves the record ID and returns the updated snapshot.
- Endpoint edits reset completed statistics, while name and activity-only edits preserve them.
- Deactivation and endpoint edits cancel an in-progress sample.
- Editing a missing ID returns structured field details.
- Malformed, negative, fractional, and overflowing IDs are rejected.
- Missing well-formed IDs produce the command-specific GET, EXISTS, DEL, and EDIT behavior defined above.
- Missing, unknown, incorrectly typed, invalid UTF-8, and oversized ADD fields are rejected with `details.command` and `details.field`.
- Address boundaries `0` and `65535`, reversed endpoints, and equal endpoints are covered.
- The maximum record count is enforced.
- Statistics freeze at `maxTestCount`.
- The sample that reaches `maxTestCount` contributes to the average and leaves no in-progress state.
- A sample beginning at clock cycle zero completes correctly.
- Pause/resume preserves an in-progress sample without adding paused wall-clock time.
- Reset, restart, and ROM load preserve completed statistics but cancel an in-progress sample.
- CodePerf mutations and snapshots work while emulation is running when advertised.

## 10. Implementation Notes

- Add `DEBUG_CODE_PERF_GET_ALL = 101` after the current public command range.
- Add `DEBUG_CODE_PERF_EDIT = 102` after GET_ALL.
- Extend supported-command bounds, `GET_SERVER_INFO.commands`, protocol documentation, and command-ID tests through 102.
- Return deterministic GET_ALL ordering by sorting IDs; do not expose unordered-map iteration order.
- Keep numeric wire snapshots separate from human-readable `AddrToStr()` output and from any legacy JSON representation.
- Perform field-only validation in `ValidateRequest`. Enforce `maxRecords`, ID exhaustion, and EDIT existence in `DebugData`, then translate typed failures to the structured errors defined above.
- Allocate an ID only after all ADD checks pass. Check exhaustion before incrementing so signed overflow is impossible.
- Represent the sample start clock with `std::optional<uint64_t>` or an equivalent explicit state instead of using `0` as the inactive sentinel.
- Cancel in-progress samples from every reset/restart path that owns a CPU clock transition, including ROM loading, without clearing completed statistics.