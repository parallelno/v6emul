# v6emul Script Overlay Protocol Design

**Status:** Implemented
**Date:** 2026-08-09

## 1. Scope

Expose Lua-created text and rectangle overlays through IPC. Lua scripts create retained overlay state in the server. `DEBUG_SCRIPT_OVERLAY_GET` returns only overlays changed since the previous request.

The server does not rasterize overlays into the emulated framebuffer. It's the client responsobility and outside this design.

## 2. Server Model

An overlay is identified by `(scriptId, itemId)`:

- `scriptId` is the server-owned script record ID.
- `itemId` is supplied by Lua and is local to that script.
- Reusing an `itemId` replaces the previous overlay, including its type.
- One script cannot replace or remove another script's overlays.

Suggested storage:

```cpp
using OverlayItems = std::unordered_map<Id, OverlayItem>;
using ScriptOverlays = std::unordered_map<Id, OverlayItems>;
```

Each retained overlay has an internal `updated` flag. A draw callback builds a validated candidate and compares it with the retained overlay:

- A new or changed overlay is stored with `updated = true`.
- An identical call does not modify the overlay or its `updated` flag.
- An overlay already marked updated remains updated until consumed.

Scripts do not remove individual overlays. Hiding an overlay is done by updating its color to zero alpha. A transparent overlay remains retained and counts toward the per-script and global item limits.

Deleting a script or setting its script-record `active` field to false removes all server overlays owned by that script. The initiating client updates its own overlay state as part of that operation.

All mutation and snapshot operations run through the serialized emulation operation path. The overlay maps do not require a mutex.

## 3. Lua API

```lua
DrawText(id, text, x, y, <color=0xFFFFFFFF>, <vectorScreenCoords=true>)
DrawRect(id, x, y, width, height, <filled=false>, <color=0xFFFFFFFF>, <vectorScreenCoords=true>)
```

Trailing parameters are optional and must be supplied in order.

### Common fields

- `id`: integer in `0..2147483647`, scoped to the executing script.
- `x`, `y`: finite numbers within `maxCoordinateMagnitude`.
- `color`: unsigned `0xRRGGBBAA` in `0..4294967295`.
- `vectorScreenCoords`: selects Vector-06C active-screen coordinates when true and complete framebuffer coordinates when false.
- `filled`: selects filled or outlined rectangle rendering.

### Coordinate spaces

When `vectorScreenCoords` is true, coordinates use the Vector-06C active screen. When false, they use the complete framebuffer.

- Non-negative `x` is measured from the left; negative `x` is measured from the right.
- Non-negative `y` is measured from the top; negative `y` is measured from the bottom.
- Width and height use the selected coordinate space's pixel units.

### Primitive rules

- `DrawText` requires valid UTF-8 text without NUL bytes.
- `DrawRect` requires non-negative width and height.
- Calling either function with an existing `id` replaces that overlay, including its type.

## 4. IPC Contract

Reserve command ID `110`:

| ID | Command | Request | Response |
|---:|---|---|---|
| 110 | `DEBUG_SCRIPT_OVERLAY_GET` | Empty object | `ScriptOverlayResponse` |

Wire types:

```ts
interface OverlayCommon {
  scriptId: number;
  itemId: number;
  vectorScreenCoords: boolean;
  x: number;
  y: number;
  color: number;
}

interface TextOverlay extends OverlayCommon {
  type: 'text';
  text: string;
}

interface RectOverlay extends OverlayCommon {
  type: 'rect';
  width: number;
  height: number;
  filled: boolean;
}

type ScriptOverlayItem = TextOverlay | RectOverlay;

interface ScriptOverlayResponse {
  overlays: ScriptOverlayItem[];
}
```

`Scripts` handles the command atomically:

1. Serialize overlays whose `updated` flag is true.
2. Clear the serialized overlays' `updated` flags.
3. Return `overlays`, including an empty array.

Unchanged overlays are not returned. Polling changes only delivery state, not retained overlay state.

`overlays` is sorted by ascending `scriptId`, then `itemId`. This is also the fixed drawing order.

The request must be an empty object. Extra fields return `invalid_request` with `details.command = 110` and the offending `details.field`.

## 5. Capabilities and Limits

Advertise command `110` and:

```ts
interface ScriptOverlayCapabilities {
  scriptOverlaySchema: 1;
  scriptOverlayRetained: true;
  scriptOverlayConsumesUpdates: true;
  scriptOverlayVectorScreenCoords: true;
  scriptOverlayColorFormat: 'RRGGBBAA';
  scriptOverlayLimits: {
    maxItemsPerScript: 256;
    maxItemsTotal: 1024;
    maxTextBytes: 4096;
    maxCoordinateMagnitude: 1000000;
  };
}
```

Reject invalid IDs, invalid UTF-8, NUL text, oversized text, out-of-range colors, NaN, infinity, invalid dimensions, mistyped parameters, and capacity exhaustion. Lua API validation and capacity failures are script runtime errors.

## 6. Lifecycle

| Event | Overlay effect |
|---|---|
| Successful script execution | Keep callback changes |
| Runtime failure or budget exhaustion | Preserve existing overlays |
| Path edit or Compile | Preserve existing overlays until the script updates them |
| Script Edit setting `active: false` | Remove overlays owned by the edited script |
| `DEBUG_SCRIPT_DISABLE` | Set the script record's `active` field to false and remove its overlays |
| Delete | Remove overlays owned by the script |
| Delete All | Remove all overlays |
| Reset, restart, or ROM load | Preserve overlays |
| Debug detach | Preserve overlays |
| Stop/start execution | Preserve overlays |
| TCP disconnect/reconnect | Preserve overlays |

Here, Script Edit means `DEBUG_SCRIPT_EDIT`, and `active` is the writable scheduling field of the script record. Disable means the separate `DEBUG_SCRIPT_DISABLE` command; both operations make that script inactive.

On a new connection, mark every retained overlay `updated = true` through the serialized hardware request path. Destroying the `Debugger` also destroys its `Scripts` instance and overlay memory, but this is object cleanup rather than an observable protocol lifecycle event.

Run Once may create overlays for an inactive script. Those overlays follow the same lifecycle rules.

## 7. Server Tests

Cover:

1. Command, schema, limits, `vectorScreenCoords`, and color format advertisement.
2. Strict empty-request validation.
3. Empty and updated-only responses.
4. Consuming per-overlay `updated` state.
5. Identical calls producing no snapshot.
6. Exact text and rectangle wire variants, including `filled`.
7. Default, partial, and complete optional arguments plus validation failures.
8. Script-scoped identity and equal item IDs in different scripts.
9. Type replacement and transparent overlay updates.
10. Deterministic overlay ordering.
11. Per-script and total capacity limits.
12. Runtime failure and budget-exhaustion preservation.
13. Disable and delete cleanup.
14. Reset, restart, ROM-load, detach, stop/start, and reconnect preservation.
15. Reconnect marking all retained overlays updated.
16. Polling while hardware is running.

## 8. Implementation Checklist

- [x] Reserve and advertise `DEBUG_SCRIPT_OVERLAY_GET = 110`.
- [x] Add overlay wire types, limits, and capability fields.
- [x] Replace `UIItem`, `UIReqs`, `UIType`, `vectorScreenCoords`, and the UI mutex.
- [x] Store overlays by `(scriptId, itemId)` with internal `updated` flags.
- [x] Implement strict positional `DrawText` and `DrawRect` callbacks with trailing optional arguments.
- [x] Publish one rectangle type with a `filled` field.
- [x] Return and consume only updated overlays.
- [x] Integrate lifecycle cleanup and reconnect initialization.
- [x] Add the server tests from Section 7.
- [x] Update public protocol and architecture documentation.
- [x] Run focused tests, the full CTest suite, and sanitizer validation.

Verification on 2026-08-14: the focused IPC suite passed 1,694 assertions,
the Release CTest suite passed 10/10 tests, and the focused IPC suite passed
under MSVC AddressSanitizer.
