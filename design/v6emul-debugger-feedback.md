# v6emul Debugger Integration Feedback

This note records issues encountered in `v6emul` while working on v6vscode project implementation. C:\Work\Programming\v6vscode\ [https://github.com/parallelno/v6vscode.git]

## Problems Encountered

### Undocumented `GET_STACK_SAMPLE` Request Contract

`GET_STACK_SAMPLE` requires a JSON payload containing the current stack pointer:

```json
{
  "addr": 65520
}
```

The native implementation directly reads `_addrJ["addr"]`. A request without this field can throw inside the emulator and appears to have caused emulation to stop or crash when Raw Stack was expanded.

### Inconsistent Response Shape

`GET_STACK_SAMPLE` returns an object whose keys are stack-relative offsets:

```json
{
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
```

It does not return `{ "data": [...] }` like several neighboring memory commands. The public command declarations expose numeric command IDs but do not describe request or response schemas, so clients must inspect the C++ implementation to discover the contract.

### Malformed Requests Can Affect Emulator Stability

Debugger requests are external input. Missing fields, incorrect JSON types, and out-of-range values should produce structured protocol errors rather than exceptions that can terminate or destabilize emulation.

### No Automated Real-Emulator DAP Scenario

The extension's `test:feature:debug` script accepts a `V6EMUL` executable but currently stops with:

> The automated real-emulator DAP scenario is not implemented yet. No result file was written.

This prevents repeatable end-to-end verification of Raw Stack, execution control, display coexistence, and shutdown against a real emulator process.

### Multiple Binaries Make Validation Ambiguous

Development can involve debug and release binaries from the `v6emul` repository as well as the extension's bundled binary. Without an exposed build identity and protocol version, it is easy to test an older or incompatible executable.

## Recommended Improvements

### 1. Validate Requests at the IPC Boundary

Validate required fields and types before dispatching commands. For example:

```cpp
if (!_addrJ.contains("addr") || !_addrJ["addr"].is_number_unsigned()) {
    return MakeError("GET_STACK_SAMPLE requires an unsigned addr");
}
```

The exact helper should follow the existing native error model. Validation should also define accepted address ranges and 16-bit wrapping behavior.

### 2. Contain Protocol Exceptions

Catch JSON parsing, missing-field, type, and command-dispatch exceptions at the top-level IPC boundary. Return a structured error response and keep both the IPC server and emulation process alive.

This is the highest-priority backend improvement: an imperfect client must not be able to crash the emulator.

### 3. Publish Typed Command Schemas

Document each IPC command with:

- Numeric command ID and stable symbolic name.
- Required and optional request fields.
- Field types and valid ranges.
- Success response shape.
- Error response shape and error codes.
- Execution-state requirements, such as paused-only commands.
- Address-space and wrapping semantics.

A machine-readable schema or generated bindings would reduce drift between C++ and TypeScript clients.

### 4. Standardize Response Envelopes

Use a consistent response envelope across commands. Raw Stack data could use an explicit array rather than numeric string keys:

```json
{
  "ok": true,
  "data": {
    "words": [
      { "offset": -10, "value": 4660 },
      { "offset": -8, "value": 22136 },
      { "offset": 0, "value": 39612 }
    ]
  }
}
```

If changing the existing response would break clients, introduce the normalized shape under a new protocol version or capability.

### 5. Expose Protocol Capabilities and Version

Add a handshake command that reports:

- Protocol version.
- Emulator version and build identifier.
- Supported commands.
- Optional debugger capabilities.
- Response schema versions where necessary.

The extension can then reject incompatible binaries with an actionable message instead of failing after launch.

### 6. Add Native Protocol Tests

At minimum, test `GET_STACK_SAMPLE` with:

- A valid stack pointer.
- Missing `addr`.
- Null, string, negative, fractional, and oversized values.
- Boundary addresses such as `0x0000` and `0xFFFF`.
- Correct words and offsets from SP-10 through SP+10.
- Requests while running and while paused, if behavior differs.

Equivalent malformed-input tests should cover every IPC command.

### 7. Add an IPC Smoke-Test Client

Provide a small test client that launches or attaches to `v6emul`, executes every read-only command, validates response schemas, and confirms malformed requests do not terminate the server.

### 8. Complete the Real-Emulator DAP Harness

The end-to-end scenario should cover:

1. Launch and handshake.
2. Pause and continue.
3. Register and flag reads.
4. Raw Stack expansion.
5. Source and instruction breakpoints.
6. Display polling while debugger requests are active.
7. One-click debug disconnect.
8. Display-tab closure terminating the debug launch.
9. Clean emulator process and socket shutdown.
10. Repeated launch and disconnect cycles.

### 9. Identify the Running Binary

Expose a useful `v6emul --version` result and return the same build identity through IPC. Test output should record the executable path, build identifier, protocol version, and enabled debugger capabilities.

## Priority

1. Defensive request validation and exception containment.
2. Protocol schema documentation and capability/version reporting.
3. Native malformed-input and boundary tests.
4. Automated real-emulator DAP coverage.
5. Response-shape normalization in a compatible protocol revision.

## Architectural Disposition

Implemented:

- Centralized app-layer request validation rejects malformed envelopes, unknown commands, and invalid `GET_STACK_SAMPLE` addresses before they enter the emulation queue.
- Exceptions raised while processing a command are relayed across the thread boundary and converted to structured protocol errors while emulation remains available.
- Error responses now include a stable `code` field while preserving the existing `ok` and `error` fields.
- `GET_SERVER_INFO` (`cmd = -5`) reports protocol version, emulator build identity, supported command IDs, and debugger capabilities.
- Native tests cover malformed stack addresses, 16-bit boundaries, exact words and offsets, paused/running access, and a valid request after a failed request.
- The public protocol documentation now defines the exact stack schema, error codes, execution-state behavior, and wrapping semantics.

Not implemented in this revision:

- The existing `GET_STACK_SAMPLE` response shape was retained. Changing it in protocol version 1 would break working clients; `stackSampleSchema: 1` makes the current shape explicit and leaves room for a versioned replacement.
- A second IPC smoke-test client was not added. The existing native IPC and end-to-end test targets are the maintainable home for backend protocol coverage; another client would duplicate transport and schema logic.
- The real-emulator DAP harness belongs to the `v6vscode` repository because DAP lifecycle and display-tab behavior are extension responsibilities. This repository now exposes the handshake and stable failure behavior that harness needs.
- A separate machine-readable schema was not added without a generation pipeline. Maintaining hand-written C++, Markdown, and schema copies would increase drift. The centralized validator is structured so command descriptors can become the source for generated bindings in a future protocol revision.
