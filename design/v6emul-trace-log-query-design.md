# V6emul Trace Log Query Interface

**Status:** Proposed
**Date:** 2026-08-07
**Consumers:** v6vscode Trace Log panel and other remote debugger clients
**Related work:** `trace-log-panel-plan.md`

## 1. Purpose

Expose retained executed-instruction history to remote clients without transferring the complete trace. The existing trace-log enable/disable commands remain file-logging controls and do not provide execution log window for the client UI panel.

## 2. Concept
Filtering and row retrieval are separate operations. A server creates a filtered result once, then provides a window of that data.

## 3. Availability

`GET_SERVER_INFO` must advertise:

```json
{
  "traceLogSchema": 1,
  "traceLogFilter": true,
  "traceLogWindowQuery": true,
  "traceLogLimits": {
    "capacity": 300000,
    "maxLines": 512,
    "maxPatternBytes": 64
  }
}
```

Add `DEBUG_TRACE_LOG_FILTER` and `DEBUG_TRACE_LOG_WINDOW` using the next available IPC command IDs. Clients without schema 1 and both commands must show an unsupported state; they must not infer trace data from logging.

## 3. Filter Contract

```ts
interface TraceLogFilterRequest {
  addressPattern?: string;
  instructionPattern?: string;
}

interface TraceLogFilterResponse {
  filterId: number;
  totalMatches: number;
}
```

Both patterns use case-insensitive `*` glob matching against canonical text. Omitted patterns match all retained instructions. Results are ordered newest first.

`filterId` identifies an immutable filtered result for the current paused state. It is an opaque positive integer for clients and ensures that delayed window requests cannot accidentally read results from a newer filter. The ID is invalidated by new filter request, emulation stop event, and step operations. A simple implementation is a monotonically increasing counter that is incremented on each filter invalidation.

`totalMatches` is the complete filtered-result size and allows the client to size its virtual scrollbar without retrieving all rows.

## 4. Window Query Contract

```ts
interface TraceLogWindowRequest {
  filterId: number;
  start: number;
  lines: number;
}

interface TraceLogWindowResponse {
  start: number;
  entries: TraceLogEntry[];
}
```

`start` is a zero-based index in the filtered result. `lines` is an application-controlled window size calculated from visible table rows plus overscan and clamped to `maxLines`.

The response repeats `start` and contains at most `lines` entries. A short or empty response at the end is valid. `totalMatches`, `start`, and `entries.length` fully describe the window.

## 5. Entry Contract

```ts
interface TraceLogEntry {
  address: number;
  bytes: number[];
  instruction: string;
}
```

- `address` is the displayed 16-bit instruction address.
- `bytes` instruction bytes including opcode and possible immediate oprand.
- `instruction` is undecorated I8080 assembly mnemonic and operands with stable case and whitespace.

## 6. Errors and limitations

Reject requests while emulation is running. Reject malformed or oversized patterns, unknown or expired filter IDs, invalid starts, invalid line limits, and unsupported schema usage with structured request errors.