# v6emul Script Overlay Protocol Design

**Status:** Proposed
**Date:** 2026-08-09

## 1. Scope

Expose Lua-created text and rectangle overlays through IPC. Lua scripts create retained overlay state in the server. `DEBUG_SCRIPT_OVERLAY_GET` returns only overlays changed since the previous request and keys of removed overlays.

The server does not rasterize overlays into the emulated framebuffer. It's the client responsobilities and outside this design.

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
using RemovedOverlayKeys = std::unordered_set<OverlayKey>;
```

Each retained overlay has an internal `updated` flag. A draw callback builds a validated candidate and compares it with the retained overlay:

- A new or changed overlay is stored with `updated = true`.
- An identical call does not modify the overlay or its `updated` flag.
- An overlay already marked updated remains updated until consumed.

Removing an overlay erases it and adds `(scriptId, itemId)` to the pending-removal set. Recreating the same key before consumption removes the pending-removal key and stores the recreated overlay with `updated = true`.

All mutation and snapshot operations run through the serialized emulation operation path. The overlay maps do not require a mutex.

## 3. Lua API

```lua
DrawText(itemId, text, x, y, options)
DrawRect(itemId, x, y, width, height, options)
DrawRectFilled(itemId, x, y, width, height, options)
RemoveDrawItem(itemId)
ClearDrawItems()
```

`options` is optional:

```lua
{
  color = 0xFFFFFFFF,
  coordinateSpace = "frame",
  zIndex = 0,
  fontSize = 12,  -- DrawText only
}
```

Unknown or inapplicable options are runtime errors.

### Common fields

- `itemId`: integer in `0..2147483647`.
- `x`, `y`: finite numbers within `maxCoordinateMagnitude`.
- `color`: unsigned `0xRRGGBBAA` in `0..4294967295`.
- `zIndex`: signed 32-bit integer; lower values are drawn first.
- `coordinateSpace`: `frame` or `normalized`.
- `(x, y)`: bottom-left origin of the text or rectangle.

### Coordinate spaces

`frame` uses complete framebuffer coordinates:

- Origin is the framebuffer bottom-left.
- Positive X points right; positive Y points up.
- Coordinates, dimensions, font size are framebuffer-pixel units.

`normalized` uses fractions of the complete framebuffer:

- Origin is the framebuffer bottom-left.
- X and width are fractions of framebuffer width.
- Y, height, font size are fractions of framebuffer height.

### Primitive rules

- `DrawText` requires valid UTF-8 text without NUL bytes. `(x, y)` is the bottom-left of the text layout box. `fontSize` must be positive.
- `DrawRect` and `DrawRectFilled` require non-negative width and height. `(x, y)` is the rectangle's bottom-left corner.
- `RemoveDrawItem` is an idempotent no-op for an unknown item.
- `ClearDrawItems` removes all overlays owned by the executing script.

## 4. IPC Contract

Reserve command ID `110`:

| ID | Command | Request | Response |
|---:|---|---|---|
| 110 | `DEBUG_SCRIPT_OVERLAY_GET` | Empty object | `ScriptOverlayResponse` |

Wire types:

```ts
type CoordinateSpace = 'frame' | 'normalized';

interface OverlayCommon {
  scriptId: number;
  itemId: number;
  zIndex: number;
  coordinateSpace: CoordinateSpace;
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
}

type ScriptOverlayItem = TextOverlay | RectOverlay;

interface ScriptOverlayKey {
  scriptId: number;
  itemId: number;
}

interface ScriptOverlayResponse {
  overlays: ScriptOverlayItem[];
  removed: ScriptOverlayKey[];
}
```

`Scripts` handles the command atomically:

1. Serialize overlays whose `updated` flag is true.
2. Serialize pending removal keys.
3. Clear the serialized overlays' `updated` flags.
4. Clear the serialized pending-removal keys.
5. Return both arrays, including empty arrays.

Unchanged overlays are not returned. Polling changes only delivery state, not retained overlay state.

`overlays` is sorted by ascending `zIndex`, `scriptId`, then `itemId`. `removed` is sorted by ascending `scriptId`, then `itemId`.

The request must be an empty object. Extra fields return `invalid_request` with `details.command = 110` and the offending `details.field`.

## 5. Capabilities and Limits

Advertise command `110` and:

```ts
interface ScriptOverlayCapabilities {
  scriptOverlaySchema: 1;
  scriptOverlayRetained: true;
  scriptOverlayConsumesUpdates: true;
  scriptOverlayCoordinateSpaces: ['frame', 'normalized'];
  scriptOverlayColorFormat: 'RRGGBBAA';
  scriptOverlayLimits: {
    maxItemsPerScript: 256;
    maxItemsTotal: 1024;
    maxTextBytes: 4096;
    maxCoordinateMagnitude: 1000000;
    maxFontSize: 512;
    maxLineWidth: 512;
  };
}
```

Reject invalid item IDs, invalid UTF-8, NUL text, oversized text, unknown options, unsupported coordinate spaces, out-of-range colors, NaN, infinity, invalid dimensions, and capacity exhaustion. Lua API validation and capacity failures are script runtime errors.

## 6. Lifecycle

| Event | Overlay effect |
|---|---|
| Successful script execution | Keep callback changes |
| Runtime failure or budget exhaustion | Remove all overlays owned by the script |
| Path edit or Compile | Remove overlays from the previous compiled generation |
| Edit setting `active: false` or Disable | Remove overlays owned by the script |
| Delete | Remove overlays owned by the script |
| Delete All | Remove all overlays |
| Debug detach | Remove all overlays |
| Stop/start execution | Preserve overlays |
| TCP disconnect/reconnect | Preserve overlays |
| Debugger destruction | Destroy overlays |

Removal operations populate the pending-removal set only when a client is connected. On a new connection, discard stale pending removals and mark every retained overlay `updated = true` through the serialized hardware request path.

Run Once may create overlays for an inactive script. Those overlays follow the same lifecycle rules.

## 7. Server Tests

Cover:

1. Command, schema, limits, coordinate spaces, and color format advertisement.
2. Strict empty-request validation.
3. Empty, updated-only, and removal responses.
4. Consuming `updated` flags and pending removals.
5. Identical calls producing no snapshot.
6. Exact text, rectangle, and filled-rectangle wire variants.
7. Defaults, explicit options, and validation failures.
8. Script-scoped identity and equal item IDs in different scripts.
9. Type replacement, explicit removal, clear, and idempotent removal.
10. Deterministic overlay and removal ordering.
11. Per-script and total capacity limits.
12. Runtime failure and budget-exhaustion cleanup.
13. Compile, disable, delete, reset, restart, ROM-load, and detach cleanup.
14. Stop/start and reconnect preservation.
15. Reconnect marking all retained overlays updated.
16. Polling while hardware is running.

## 8. Implementation Checklist

- [ ] Reserve and advertise `DEBUG_SCRIPT_OVERLAY_GET = 110`.
- [ ] Add overlay wire types, limits, and capability fields.
- [ ] Replace `UIItem`, `UIReqs`, `UIType`, `vectorScreenCoords`, and the UI mutex.
- [ ] Store overlays by `(scriptId, itemId)` with internal `updated` flags.
- [ ] Track pending removal keys.
- [ ] Implement strict `DrawText`, `DrawRect`, and `DrawRectFilled` callbacks.
- [ ] Implement `RemoveDrawItem` and `ClearDrawItems`.
- [ ] Return and consume only updated overlays and pending removals.
- [ ] Integrate lifecycle cleanup and reconnect initialization.
- [ ] Add the server tests from Section 7.
- [ ] Update public protocol and architecture documentation.
- [ ] Run focused tests, the full CTest suite, and sanitizer validation.
