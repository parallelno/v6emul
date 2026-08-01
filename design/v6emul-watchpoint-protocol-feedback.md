# v6emul Watchpoint Protocol Feedback from v6vscode

**Status:** Client integration feedback
**Date:** 2026-07-31
**Consumer:** v6vscode
**Related plan:** `watchpoints-panel-plan.md`

## 1. Scope

This document describes what is unreliable or missing in the watchpoint IPC contract from the perspective of v6vscode. It deliberately does not prescribe v6emul's internal classes, containers, threading, or algorithms. The implementation is owned by v6emul; v6vscode needs the externally visible behavior below.

The current watchpoint commands are:

| ID | Command |
|---:|---|
| 69 | `DEBUG_WATCHPOINT_ADD` |
| 70 | `DEBUG_WATCHPOINT_DEL_ALL` |
| 71 | `DEBUG_WATCHPOINT_DEL` |
| 72 | `DEBUG_WATCHPOINT_GET_UPDATES` |
| 73 | `DEBUG_WATCHPOINT_GET_ALL` |

## 2. Summary of Client Problems

The current contract requires v6vscode to understand packed C++ object layout and the special ID value used for creation, accepts malformed watchpoint data too late, exposes transient execution state as editable configuration, and does not identify which watchpoint stopped execution.

For reliable integration, v6vscode needs:

1. A structured, versioned watchpoint schema.
2. `DEBUG_WATCHPOINT_ADD` without an `id` field or sentinel value.
3. Explicit validation errors for invalid requests.
4. Stable list responses.
5. Watchpoint stop details, including the watchpoint ID and memory access.
6. Advertised capabilities and limits.
7. Black-box protocol tests and fixtures that v6vscode can reuse.

## 3. Packed Watchpoint Data Is Not a Safe Client Contract

### Current behavior

`DEBUG_WATCHPOINT_ADD` and `DEBUG_WATCHPOINT_GET_ALL` exchange `data0` and `data1` as packed 64-bit words derived from `Watchpoint::Data`. Their meaning depends on C++ field sizes, packing, endianness, enum values, and bitfield layout. The words also include `breakL` and `breakH`, which are transient execution state.

### Client impact

v6vscode would have to:

- Reproduce compiler-dependent C++ layout in TypeScript.
- Decode and encode full-width integers as `bigint` rather than normal JavaScript numbers.
- Maintain golden values for each supported v6emul build toolchain.
- Risk sending transient hit-state bits back during an unrelated edit.
- Change whenever the native object layout changes, even if the conceptual watchpoint model does not.

This makes the native in-memory representation part of the public protocol and prevents the client and server from evolving independently.

### Required contract

v6vscode needs a structured watchpoint representation with explicit fields:

```json
{
  "id": 17,
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

Requirements:

- Configuration fields have documented names, types, ranges, and enum values.
- `breakL` and `breakH` are not configuration fields and cannot be submitted by a client.
- The schema is versioned and advertised by `GET_SERVER_INFO`.
- Packed watchpoint values are not supported by the new contract.

## 4. Watchpoint Creation Must Not Require a Client ID

### Current behavior

`DEBUG_WATCHPOINT_ADD` requires the client to send `id: -1` for a new watchpoint. The backend recognizes this sentinel and replaces it with a unique ID.

### Client impact

The special value `-1` is a backend convention that leaks into every client. A client should describe the requested watchpoint, not participate in ID allocation or know the sentinel used to request it.

### Required contract

The new watchpoint protocol must define `DEBUG_WATCHPOINT_ADD` without an `id` field. The backend assigns the unique ID. The assigned ID becomes visible through `DEBUG_WATCHPOINT_GET_ALL` and is used by later update or delete requests.

The `id: -1` convention is not part of the client contract and is not accepted by the server.

## 5. Invalid Requests Need Stable Validation Errors

### Current behavior

Watchpoint fields are not described in `docs/ipc-protocol.md` beyond “watchpoint definition.” Invalid or missing watchpoint fields can reach command dispatch and produce generic errors.

### Client impact

v6vscode cannot reliably distinguish:

- A user input error.
- An unsupported schema or enum.
- A missing watchpoint.
- An internal emulator failure.

Without field-level constraints, client-side validation can also disagree with v6emul.

### Required contract

The protocol must document and validate:

- `id` type and valid use by operation.
- `globalAddr` bounds.
- `len` minimum, maximum, and overflow behavior.
- `value` bounds for `LEN` and `WORD`.
- Valid Access, Condition, and Type values.
- The `WORD` length rule.
- Comment encoding and maximum UTF-8 byte length.
- Maximum watchpoint count.
- Whether mutation is accepted while the emulator is running.
- Whether unknown or extra fields are rejected.

Invalid requests must return `invalid_request` with the command and failing field. A validation failure must not mutate watchpoint state, disconnect the client, or prevent the next valid request from succeeding.

## 6. Mutations Need Validation Errors

### Current behavior

The client already knows the watchpoint values it sends in `DEBUG_WATCHPOINT_ADD` and the ID it sends in `DEBUG_WATCHPOINT_DEL`. It does not need the server to echo the complete watchpoint after every successful mutation.

### Client impact

The reliability problem is not the absence of a canonical response. It is the absence of watchpoint-specific validation and clear errors when a mutation is invalid, similarly to the breakpoint protocol.

### Required contract

`DEBUG_WATCHPOINT_ADD`, `DEBUG_WATCHPOINT_DEL`, and `DEBUG_WATCHPOINT_DEL_ALL` should return normal success when accepted. Invalid data, an unknown ID, or an unsupported operation should return a stable protocol error with enough detail for v6vscode to report or correct the problem. No echoed watchpoint is required.

## 7. Collection Synchronization Needs One Stable Shape

### Current behavior

`DEBUG_WATCHPOINT_GET_ALL` returns null when empty and an array when populated. Entry order is unspecified. `DEBUG_WATCHPOINT_GET_UPDATES` returns a `uint32_t` mutation counter.

### Client impact

v6vscode must normalize multiple collection shapes and sort unstable snapshots. The update counter is useful, but its wrap and no-op behavior is not documented.

### Required contract

v6vscode needs:

- An empty collection represented as `[]`.
- A documented deterministic order, preferably by ID or `(globalAddr, id)`.
- A documented update-counter width and wrap policy.
- A documented rule for whether rejected or no-op operations change the update counter.
- `DEBUG_WATCHPOINT_GET_UPDATES` semantics documented as the lightweight invalidation mechanism.

## 8. Bulk Operations Need Explicit Semantics

### Current behavior

`DEBUG_WATCHPOINT_DEL_ALL` deletes every watchpoint. Disabling all watchpoints requires `DEBUG_WATCHPOINT_ADD` once per entry. The protocol does not define a bulk disable operation.

### Client impact

Repeated per-row updates can leave a partially changed collection when one request fails.

### Required contract

v6vscode needs:

- Confirmation that `DEBUG_WATCHPOINT_DEL_ALL` applies to the complete shared watchpoint collection.
- A bulk enable/disable request so Disable All is one validated operation rather than a sequence of replacements.

## 9. Watchpoint Matching Semantics Need Documentation

### Current behavior

The protocol documentation does not define what `LEN`, `WORD`, Access, and Condition mean. Source inspection shows that `LEN` compares matching bytes in a range and `WORD` uses low/high byte match state. That behavior is not a stable public contract.

### Client impact

v6vscode cannot explain to users exactly when a watchpoint will fire or validate fields confidently. Different client implementations may present the same watchpoint differently.

### Required contract

Document and test:

- The exact numeric or string values for every enum.
- Whether comparison is signed or unsigned.
- Whether `ANY` ignores `value`.
- Whether `LEN` compares every accessed byte independently.
- The byte order used by `WORD`.
- Whether WORD halves must be accessed by one instruction or may match across instructions.
- Read, write, and read/write access classification.
- Behavior for overlapping watchpoints.
- Behavior at the end of global memory.
- Configuration and hit-state behavior across reset, restart, attach, detach, reconnect, ROM load, and process restart.

The chosen semantics are owned by v6emul; v6vscode only needs them to be explicit and stable.

## 10. Stop Events Need Watchpoint Attribution

### Current behavior

The externally visible debugger state does not reliably tell v6vscode which watchpoint caused execution to stop or which memory access matched it.

### Client impact

v6vscode cannot produce an accurate DAP data-breakpoint stopped event, highlight the triggering watchpoint, or explain the matching access. Polling the collection does not recover this information.

### Required contract

After a watchpoint stop, v6vscode needs a non-consuming stop record containing:

- A monotonic stop sequence.
- Stop reason.
- CPU PC and global instruction address.
- Triggering watchpoint ID or all triggering IDs.
- Access type.
- Accessed global address.
- Observed value.
- Previous and new values for writes when available.

The record must remain available until superseded. The watchpoint update counter and execution-stop sequence must remain separate.

## 11. Capabilities and Limits Must Be Discoverable

Command presence alone is not enough to identify the contract supported by a particular v6emul build. `GET_SERVER_INFO` should let v6vscode discover:

- Structured watchpoint schema version.
- ID-free `DEBUG_WATCHPOINT_ADD` support.
- Batch and bulk-enable/disable support.
- Reliable watchpoint stop attribution.
- Whether mutations are safe while running.
- Maximum watchpoint count, range length, and comment bytes.

This allows v6vscode to enable only operations that the connected emulator can perform reliably.

## 12. Updated/New Tests for this update

## 13. v6emul Architectural Disposition

The feedback correctly identifies native object serialization as the principal architectural defect. The implementation accepts recommendations that decouple the wire contract from C++ layout and make existing behavior deterministic, but does not add narrowly scoped commands where the underlying debugger abstraction is not yet designed.

| Recommendation | Decision | Rationale |
|---|---|---|
| Structured, versioned schema | Accepted | Removes compiler layout, endianness, and transient state from the public contract. Commands 69 and 73 use schema 1 exclusively. |
| ID-free creation | Accepted | ID allocation belongs to the collection. Clients cannot submit an ID when creating a watchpoint. |
| Field validation and stable errors | Accepted for schema 1 | Validation occurs before emulation-thread dispatch and rejects unknown fields. Unknown-ID mutation errors require a typed command-result design and are not emulated with ad hoc response fields. |
| Stable list shape and order | Accepted | All snapshots are arrays, including empty snapshots, and are ordered by ID. |
| Update-counter semantics | Accepted | The counter is documented as wrapping `uint32`; rejected and no-op mutations do not increment it. |
| Capability and limit discovery | Accepted for implemented behavior | Schema, server ID allocation, running mutation support, range length, and comment length are advertised. No unsupported maximum count is claimed. |
| Bulk enable/disable | Deferred | A one-off bulk toggle would duplicate mutation rules. This should be designed as a validated transactional batch facility shared by debugger collections. |
| Stop attribution | Deferred | Stop metadata should be a unified debugger stop record covering breakpoints, watchpoints, halts, and exceptions, not a watchpoint-only side channel. |
| Matching and lifecycle semantics | Partially accepted | Existing matching and counter behavior is documented. Reset/reconnect/process-lifetime policy needs a broader debugger lifecycle contract before being promised. |
| Reusable external fixtures | Deferred | In-repository black-box contract tests are added first. Cross-repository fixtures should follow only if both projects adopt a versioned fixture package and compatibility policy. |

This disposition intentionally leaves command IDs 69-73 stable while removing packed requests and responses completely. Clients should require `capabilities.watchpointSchema == 1`.
