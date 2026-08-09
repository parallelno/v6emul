# v6emul Script Overlay Protocol Design

**Status:** Proposed
**Date:** 2026-08-08
**Consumer:** v6vscode and other IPC clients

## 1. Scope

Define a versioned server-to-client protocol for drawing script-produced text and rectangles over the emulated display.

Lua scripts remain the only writers. A script calls `DrawText`, `DrawRect`, or `DrawRectFilled`; the server stores a retained overlay scene; an IPC client pulls a coherent snapshot and renders it locally. The server does not rasterize script overlays into the emulated framebuffer.

This design replaces the unused `Scripts::UIItem`, `UIReqs`, and `vectorScreenCoords` contract. Those types came from the old in-process ImGui frontend and are not a public compatibility surface.

The design does not provide arbitrary client-to-server drawing, input handling, images, fonts, paths, animation timelines, or general UI widgets.

## 2. Current State and Problems

The current Lua callbacks in `Scripts::RegisterCppFunctions()` write retained items into `m_uiReqs`, and `GetUIItems()` returns a mutex-protected copy. No public hardware request serializes this state, so remote clients cannot consume it.

The old model is not suitable as a wire contract:

- Item identity is only the Lua-provided `id`. Two scripts using the same ID overwrite each other and transfer ownership.
- `vectorScreenCoords` is a frontend-specific boolean with undocumented wire geometry.
- The old viewport-coordinate renderer depends on an in-process window position, size, font, and DPI that the server cannot know.
- Items have no deterministic drawing order.
- Overlay changes have no revision independent of the script-record revision.
- Text and item counts are unbounded.
- Coordinates accept NaN and infinity through `luaL_checknumber`.
- Scripts cannot explicitly remove one retained item or clear their own output.
- Lua callbacks mutate live state directly, so there is no explicit invocation-level commit model.
- Successful recompilation can leave output produced by the previous compiled generation.

## 3. Design Decisions

### 3.1 Retained scene

The overlay is retained state, not an immediate-mode command stream. Calling a draw function creates or replaces an item. An item remains until its owning script replaces or removes it, the script is cleaned up, or a global lifecycle event clears the overlay.

A retained scene avoids sending every Lua call over IPC and lets a client reconnect or recover from a missed poll by requesting one complete snapshot.

### 3.2 Script-scoped identity

An item is identified by the pair `(scriptId, itemId)`.

- `scriptId` is the server-owned script record ID.
- `itemId` is selected by Lua and is local to that script.
- One script cannot replace or remove another script's items.
- Reusing an `itemId` in the same script replaces the previous item, including its primitive type.

The recommended core representation is an owner-partitioned map:

```cpp
using OverlayItems = std::unordered_map<Id, OverlayItem>;
using ScriptOverlays = std::unordered_map<Id, OverlayItems>;
```

This makes per-script capacity checks, cleanup, and transaction staging direct. The IPC snapshot is sorted and must not expose unordered-map iteration order.

### 3.3 Separate overlay revision

Overlay state has a dedicated wrapping `uint64` revision named `overlayUpdates`. It is independent of the existing script collection `updates` counter.

Script status changes are low-frequency management state; overlay text may change every invocation. Reusing one counter would force clients to refetch unrelated script records.

The revision increments once when one completed operation changes the final observable overlay snapshot. Multiple draw calls in one Lua invocation increment it at most once. Identical replacements do not increment it.

### 3.4 Pull protocol with conditional snapshot

The existing IPC architecture is request/response and clients already pull display frames. Use one conditional snapshot command instead of a two-round-trip `GET_UPDATES` then `GET_ALL` sequence. Two requests can cross two emulation request boundaries and add a frame of avoidable latency.

Reserve the next public command ID:

| ID | Command | Request | Successful data |
|---:|---|---|---|
| 110 | `DEBUG_SCRIPT_OVERLAY_GET` | Empty object or `{ "knownUpdates": uint64 }` | `ScriptOverlayResponse` |

Initial or unconditional request:

```json
{}
```

Conditional request:

```json
{ "knownUpdates": 42 }
```

Unchanged response:

```json
{
  "overlayUpdates": 42,
  "changed": false
}
```

Changed or unconditional response:

```json
{
  "overlayUpdates": 43,
  "changed": true,
  "items": []
}
```

`items` is present exactly when `changed` is true. An unconditional request always returns `changed: true` and a complete `items` array, including an empty array.

A client compares revisions for equality only. It must not infer ordering from a wrapping revision. After connection, reconnect, or local state loss, it sends an unconditional request.

### 3.5 Client-side rendering

The server publishes logical primitives. The client controls rasterization, DPI scaling, clipping, antialiasing, and the actual font family. Script overlay output is not part of `GET_FRAME` or `GET_FRAME_RAW`, so clients may hide it without changing emulation output.

A client should request the conditional snapshot at its display refresh cadence or after receiving a new frame. It replaces its entire local overlay scene whenever `changed` is true. It must clear local overlay state on disconnect before a new unconditional snapshot is received.

## 4. Capabilities

Advertise command `110` in `GET_SERVER_INFO.commands` and add:

```ts
interface ScriptOverlayLimits {
  maxItemsPerScript: number;
  maxItemsTotal: number;
  maxTextBytes: number;
  maxCoordinateMagnitude: number;
  maxFontSize: number;
  maxLineWidth: number;
}

interface ScriptOverlayCapabilities {
  scriptOverlaySchema: 1;
  scriptOverlayRetained: true;
  scriptOverlayConditionalSnapshot: true;
  scriptOverlayCoordinateSpaces: ['frame', 'normalized'];
  scriptOverlayColorFormat: 'RRGGBBAA';
  scriptOverlayLimits: ScriptOverlayLimits;
}
```

Initial server limits:

| Limit | Value |
|---|---:|
| `maxItemsPerScript` | 256 |
| `maxItemsTotal` | 1024 |
| `maxTextBytes` | 4096 |
| `maxCoordinateMagnitude` | 1000000 |
| `maxFontSize` | 512 |
| `maxLineWidth` | 512 |

Clients must require `scriptOverlaySchema = 1` and command `110` before using the feature. `scriptSchema = 1` alone does not imply overlay support.

## 5. Lua API

The old positional boolean is removed. Optional rendering properties use a Lua table so schema 1 can be extended without adding more positional parameters.

```lua
DrawText(itemId, text, x, y, options)
DrawRect(itemId, x, y, width, height, options)
DrawRectFilled(itemId, x, y, width, height, options)
RemoveDrawItem(itemId)
ClearDrawItems()
```

`options` is optional. Supported keys are:

```lua
{
  color = 0xFFFFFFFF,
  coordinateSpace = "frame",
  anchor = "topLeft",
  zIndex = 0,
  fontSize = 12,  -- DrawText only
  lineWidth = 1   -- DrawRect only
}
```

Unknown option keys and options that do not apply to the primitive are Lua runtime errors. Strict options avoid silently ignoring misspellings.

### 5.1 Common arguments

- `itemId` is an integer in `0..2147483647` and is scoped to the currently executing script.
- `x` and `y` are finite numbers within the advertised coordinate bound.
- `color` is an unsigned integer in `0..4294967295`, encoded as `0xRRGGBBAA`.
- `zIndex` is a signed 32-bit integer. Lower values are drawn first.
- `coordinateSpace` is `frame` or `normalized`.
- `anchor` is `topLeft`, `topRight`, `bottomLeft`, `bottomRight`, or `center` and selects which point of the primitive is placed at `(x, y)`.

### 5.2 Coordinate spaces

`frame` uses logical coordinates of the complete framebuffer returned by `GET_FRAME_RAW`:

- Origin is the framebuffer top-left.
- Positive X points right.
- Positive Y points down.
- Coordinates, dimensions, font size, and line width are framebuffer-pixel units.
- The client applies the same scale and offset used to display the frame.

`normalized` uses the displayed framebuffer rectangle:

- Origin is the framebuffer top-left.
- X and width use fractions of framebuffer width.
- Y, height, font size, and line width use fractions of framebuffer height.
- Typical visible values are in `0..1`, but finite values outside that interval are allowed and clipped by the client.

Using the framebuffer rectangle rather than the entire client window keeps output portable across VS Code layouts, standalone clients, DPI settings, and window sizes.

### 5.3 Primitive-specific rules

`DrawText` requires valid UTF-8 text of at most `maxTextBytes`. Embedded NUL bytes are rejected. `fontSize` must be finite, positive, and at most `maxFontSize` in `frame` space or `1` in `normalized` space. Text alignment follows `anchor`.

`DrawRect` and `DrawRectFilled` require finite non-negative width and height. `DrawRect` additionally requires a finite positive `lineWidth` within its advertised bound. Rectangle anchoring applies to the complete rectangle.

`RemoveDrawItem` is an idempotent no-op when the current script does not own that item ID.

`ClearDrawItems` removes every item owned by the current script. It cannot affect another script.

## 6. Wire Model

The response uses strict tagged variants. Common fields appear on every item; irrelevant fields are omitted.

```ts
type CoordinateSpace = 'frame' | 'normalized';
type OverlayAnchor = 'topLeft' | 'topRight' | 'bottomLeft' | 'bottomRight' | 'center';

interface OverlayCommon {
  scriptId: number;
  itemId: number;
  zIndex: number;
  coordinateSpace: CoordinateSpace;
  anchor: OverlayAnchor;
  x: number;
  y: number;
  color: number;
}

interface TextOverlay extends OverlayCommon {
  type: 'text';
  text: string;
  fontSize: number;
}

interface RectOverlay extends OverlayCommon {
  type: 'rect';
  width: number;
  height: number;
  lineWidth: number;
}

interface RectFilledOverlay extends OverlayCommon {
  type: 'rectFilled';
  width: number;
  height: number;
}

type ScriptOverlayItem = TextOverlay | RectOverlay | RectFilledOverlay;

type ScriptOverlayResponse =
  | { overlayUpdates: number; changed: false }
  | { overlayUpdates: number; changed: true; items: ScriptOverlayItem[] };
```

Snapshot order is deterministic:

1. Ascending `zIndex`.
2. Ascending `scriptId`.
3. Ascending `itemId`.

This order is also the drawing order. Later items render over earlier items.

All wire numbers must be finite. MessagePack unsigned values are used for `color` and `overlayUpdates` so the full ranges survive transport.

## 7. Invocation Transactions and Revision Semantics

Each Lua invocation is one overlay transaction.

1. Before running a script, copy that script's retained items into a staging map.
2. Draw, remove, and clear callbacks mutate only the staging map.
3. Enforce per-script and total capacity against the staged result.
4. On successful Lua completion, replace that script's live item set with the staged set.
5. On Lua runtime failure, discard staging and remove all live items owned by that script.
6. Compare the final live owner set with the set from before the invocation.
7. Increment `overlayUpdates` once only when the final observable overlay changed.

No IPC request is serviced during a Lua invocation because execution and requests are serialized on the emulation operation path. A client therefore cannot observe partial callback output. Staging still makes success, failure, capacity handling, and future refactoring explicit.

The following operations use the same one-increment rule:

| Operation | Overlay effect |
|---|---|
| Successful scheduled run or Run Once | Commit staged output |
| Runtime failure or budget exhaustion | Remove all output owned by the failing script |
| Path edit or explicit Compile | Remove output from the previous compiled generation, whether compilation succeeds or fails |
| Edit setting `active: false` | Remove output owned by that script |
| Disable | Remove output even if Activity was already false |
| Delete | Remove output owned by the deleted script |
| Delete All | Clear all output |
| Reset, restart, or ROM load | Clear all output |
| Debug detach | Clear all output |
| Debugger destruction | Destroy all output |
| TCP disconnect/reconnect | Preserve server output while the same debugger exists |
| Stop/start execution | Preserve output |

Run Once may publish output for an inactive script. That output remains retained until the script changes or removes it, Disable is requested, compilation replaces the generation, the script is deleted, or a global lifecycle event clears it.

A script-record `updates` revision and `overlayUpdates` may both change during one operation. They remain independent and each advances according to its own observable state.

## 8. Validation and Failures

Lua API validation failures use `luaL_error`, become normal script runtime errors, and follow runtime-failure cleanup. Error messages identify the function and invalid argument or option.

Reject:

- Missing or extra positional arguments.
- Unknown or inapplicable option fields.
- Negative or out-of-range item IDs.
- Invalid UTF-8, embedded NUL, or oversized text.
- NaN, infinity, out-of-bound coordinates, dimensions, font sizes, or line widths.
- Negative dimensions, non-positive font size, or non-positive line width.
- Unsupported coordinate spaces or anchors.
- Colors outside the unsigned 32-bit range.
- Per-script or global item-capacity exhaustion.

Capacity exhaustion is a script runtime error; it must not partially commit staged output.

`DEBUG_SCRIPT_OVERLAY_GET` accepts exactly zero fields or exactly one `knownUpdates` unsigned integer field. Extra fields and invalid integer ranges return the normal `invalid_request` envelope with `details.command = 110` and the offending `details.field`.

## 9. Concurrency and Ownership

Lua execution, overlay mutation, lifecycle cleanup, snapshot construction, and revision changes run on the emulation operation path. The IPC server obtains state through `Hardware::Request()` and does not access `Scripts` directly.

Once the obsolete in-process renderer path is removed, the overlay maps do not need a mutex for protocol correctness. If another in-process consumer is introduced, it must use the same serialized request path rather than retaining references into live maps.

Snapshot construction copies values into JSON before returning. It never returns references, pointers, or iterators into overlay state.

## 10. Client Rendering Requirements

A conforming client:

1. Negotiates `scriptOverlaySchema = 1`, command `110`, limits, coordinate spaces, and color format.
2. Sends an unconditional snapshot request after connection.
3. Stores the returned `overlayUpdates` and complete item array.
4. Sends conditional requests with its last stored revision at its chosen display cadence.
5. Replaces the complete local item array only when `changed` is true.
6. Draws items in response order and clips them to the displayed framebuffer rectangle.
7. Clears local output immediately on disconnect or when overlay capability disappears.
8. Sends an unconditional request after reconnect rather than assuming the old local revision is valid for a new server lifetime.

Clients may omit overlay rendering entirely. They must not interpret absence of the capability as a script failure.

## 11. Server Tests

Cover:

1. Command `110`, schema, limits, coordinate spaces, and color format advertisement.
2. Strict empty/conditional request validation and structured errors.
3. Empty unconditional, unchanged conditional, and changed conditional responses.
4. Exact wire variants for text, outline rectangle, and filled rectangle.
5. Default and explicit Lua options.
6. Deterministic ordering by z-index, script ID, and item ID.
7. Same-script upsert, primitive-type replacement, remove, clear, and idempotent no-ops.
8. Equal item IDs in different scripts remaining independent.
9. Multiple callback changes producing exactly one overlay revision increment per invocation.
10. Identical final output producing no revision increment.
11. Failed and budget-exhausted invocations committing no partial output and removing prior owner output.
12. Inactive Run Once publishing retained output and Disable clearing it.
13. Successful and failed compile clearing output from the previous generation.
14. Disable, delete, delete-all, reset, restart, ROM-load, detach, and debugger-destruction cleanup.
15. Stop/start and TCP reconnect preservation.
16. Text UTF-8, NUL, and byte limits.
17. Item ID, color, coordinate, dimension, font-size, line-width, anchor, and option validation.
18. Per-script and total capacity boundaries and recovery after removal.
19. Conditional polling while hardware is running.
20. Focused tests under the existing CTest timeout and MSVC AddressSanitizer configuration.

## 12. Implementation Plan

### Phase 1: Protocol and model

1. Reserve public `Hardware::Req` value `110` as `DEBUG_SCRIPT_OVERLAY_GET`.
2. Add overlay constants, tagged item types, owner-partitioned maps, staging state, and a dedicated `uint64_t overlayUpdates` to `Scripts`.
3. Replace `UIItem`, `UIReqs`, `UIType`, `vectorScreenCoords`, `GetUIItems()`, and the UI mutex with the overlay model.
4. Add deterministic JSON serialization and conditional snapshot construction.

### Phase 2: Lua producer API

1. Replace the three old callback parsers with strict schema-1 callback helpers.
2. Add shared parsing for item ID, finite numbers, color, coordinate space, anchor, z-index, and options.
3. Implement `DrawText`, `DrawRect`, `DrawRectFilled`, `RemoveDrawItem`, and `ClearDrawItems` against invocation staging.
4. Add begin, commit, and abort transaction handling around `RunScript()`.
5. Ensure every Lua error path removes the debug hook, resets current execution context, and resolves the overlay transaction.

### Phase 3: Lifecycle integration

1. Clear owner output on compile generation replacement, disable, and delete.
2. Clear all output on delete-all, reset, restart, ROM load, debug detach, and debugger destruction.
3. Preserve output across stop/start and TCP reconnect.
4. Advance `overlayUpdates` exactly once for each operation that changes final overlay state.

### Phase 4: IPC exposure

1. Route command `110` through `Debugger::ReqHandling()` on the emulation path.
2. Extend `IsSupportedCommand`, strict request validation, and server command advertisement.
3. Add `scriptOverlaySchema`, behavior flags, coordinate spaces, color format, and limits to `GET_SERVER_INFO`.
4. Translate malformed requests through the normal structured IPC error envelope.

### Phase 5: Verification and documentation

1. Add the complete test matrix from Section 11.
2. Update `docs/ipc-protocol.md`, `docs/architecture.md`, and test-client guidance.
3. Provide client-side TypeScript model examples and frame-coordinate mapping guidance.
4. Run the focused IPC suite, full Release CTest suite, editor dashboard invocation, and MSVC AddressSanitizer focused suite.

## 13. Implementation Checklist

- [ ] Reserve and advertise `DEBUG_SCRIPT_OVERLAY_GET = 110`.
- [ ] Add `scriptOverlaySchema = 1`, behavior flags, coordinate spaces, color format, and limits.
- [ ] Replace obsolete UI types and mutex-protected global item IDs with script-scoped retained overlay state.
- [ ] Add the independent `uint64` `overlayUpdates` revision.
- [ ] Implement invocation staging, successful commit, failure cleanup, and one-increment semantics.
- [ ] Implement strict options-based `DrawText`, `DrawRect`, and `DrawRectFilled`.
- [ ] Implement `RemoveDrawItem` and `ClearDrawItems`.
- [ ] Enforce UTF-8, finite-number, range, option, and resource-limit validation.
- [ ] Implement deterministic tagged-variant snapshot serialization.
- [ ] Implement unconditional and conditional overlay snapshot responses.
- [ ] Integrate compile, disable, delete, reset, restart, ROM-load, detach, and destruction cleanup.
- [ ] Preserve output across stop/start and reconnect.
- [ ] Add all server tests from Section 11.
- [ ] Update public protocol, architecture, and client documentation.
- [ ] Run focused, full-suite, editor-dashboard, and sanitizer validation.
