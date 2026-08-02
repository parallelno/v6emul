# v6emul Hardware Statistics Design

**Status:** Implemented
**Date:** 2026-08-01
**Consumer:** v6vscode

## 1. Goal

Provide one coherent, versioned snapshot of hardware state for debugger and hardware-inspection clients without reusing the converted palette fields in legacy command 45.

The design also provides narrow stopped-state mutations for editing one raw palette register and dismounting one floppy drive. Clients verify both mutations by reading a new hardware snapshot.

## 2. Protocol Additions

The assigned command IDs are:

| ID | Command | Schema |
|---:|---|---:|
| 96 | `GET_HARDWARE_STATS` | 1 |
| 97 | `SET_IO_PALETTE_ENTRY` | 1 |
| 98 | `DISMOUNT_FDD` | 1 |

These are new named commands. Command 45, `GET_HW_MAIN_STATS`, retains its legacy response and converted palette values.

## 3. Hardware Statistics Snapshot

### Request

```json
{}
```

Unknown fields are rejected.

### Response

```json
{
  "sessionId": 7,
  "uptimeMs": 123456,
  "cpuCycles": 370368000,
  "lastRunCycles": 19874,
  "rasterPixel": 320,
  "rasterLine": 120,
  "frameCycles": 23120,
  "frameNumber": 6543,
  "displayMode": 1,
  "scrollVertical": 254,
  "rusLat": true,
  "inte": true,
  "iff": false,
  "hlta": false,
  "palette": [0, 1, 2, 3, 4, 5, 6, 7, 64, 65, 66, 67, 252, 253, 254, 255],
  "ramDisk": { "index": 0, "mapping": 176 },
  "fdc": {
    "selectedDrive": 0,
    "drives": [
      { "mounted": true, "path": "C:/disks/system.fdd", "updated": false },
      { "mounted": false, "path": "", "updated": false },
      { "mounted": false, "path": "", "updated": false },
      { "mounted": false, "path": "", "updated": false }
    ]
  }
}
```

### Snapshot Semantics

`GET_HARDWARE_STATS` is valid while hardware is stopped or running. Dispatch occurs on the emulation thread, so all response fields are captured at one request boundary without another hardware operation interleaving its fields. While running, the request is handled at an instruction boundary and does not stop execution.

The response guarantees:

- `palette` contains exactly 16 raw Vector-06C `BBGGGRRR` bytes. Values are not converted to ABGR or RGB.
- `fdc.drives` contains exactly four entries in drive-index order.
- `fdc.selectedDrive` is the selected-drive register even when that drive has no mounted media.
- `frameCycles` is the current raster position expressed in CPU cycles within the frame.
- `ramDisk.index` and `ramDisk.mapping` come from the same hardware snapshot as the CPU, display, palette, and FDC fields.

Running snapshots expose the state observed at dispatch time. `lastRunCycles` remains the previously completed run's latched value until the current run transitions to stopped.

## 4. Session and Counter Semantics

A new accepted TCP connection starts a new statistics session:

- `sessionId` changes monotonically.
- `uptimeMs`, `cpuCycles`, `frameNumber`, and `lastRunCycles` reset for the new session.
- Emulator `RESET` and `RESTART` preserve the session and its accumulated counters.

`lastRunCycles` is latched when execution transitions from running to stopped. Repeated snapshots while stopped return the same value. Starting and stopping another run replaces it with that run's cycle count.

Counters are maintained as 64-bit unsigned values. Schema 1 emits only MessagePack integers no greater than JavaScript's maximum safe integer, 9,007,199,254,740,991. The request fails before returning a larger value. A future schema must define bigint encoding before this limit can be exceeded.

## 5. Set Palette Entry

### Request

```json
{ "index": 0, "hwColor": 255 }
```

### Response

```json
{ "index": 0, "hwColor": 255 }
```

Validation rules:

- The request contains exactly `index` and `hwColor`.
- `index` is an integer from 0 through 15.
- `hwColor` is an integer from 0 through 255.
- Hardware must be stopped.

The command updates the selected raw palette byte immediately. It does not alter normal palette commit timing or pending I/O behavior. The client verifies the resulting byte through `GET_HARDWARE_STATS`.

## 6. Dismount FDD

### Request

```json
{ "driveIdx": 0 }
```

### Response

```json
{ "driveIdx": 0, "mounted": false }
```

Validation rules:

- The request contains exactly `driveIdx`.
- `driveIdx` is an integer from 0 through 3.
- Hardware must be stopped.

Dismounting an already empty drive succeeds. Dismount clears mounted state and path but does not save, discard, or clear dirty disk data. The client must resolve dirty state before requesting dismount.

If the selected drive is dismounted, the selected-drive register remains unchanged and controller reads behave as no media present. The client verifies mounted state and path through `GET_HARDWARE_STATS`.

## 7. Capability Discovery

`GET_SERVER_INFO` advertises all three command IDs and:

```json
{
  "capabilities": {
    "hardwareStatsSchema": 1,
    "hardwareStatsWhileRunning": true,
    "paletteEntryMutation": true,
    "fddDismount": true,
    "runningHardwareMutations": false
  }
}
```

Clients must check the command list and relevant capability before issuing a command. Absence of `hardwareStatsSchema: 1` means command 45 must not be interpreted as this schema.

## 8. Error Behavior

Malformed data is rejected at the IPC validation boundary with `invalid_request` and field details where applicable. Validation failure does not mutate hardware.

Stopped-state violations for mutation commands and safe-integer overflow are rejected during hardware dispatch with `dispatch_error`. A failed command does not disconnect the client or prevent a subsequent valid request.

## 9. Implementation Notes

The implementation follows this design with these concrete choices and known differences:

- Snapshot atomicity is provided by serialized request handling on the emulation thread rather than a separate hardware-state mutex. The snapshot is still assembled without another hardware mutation interleaving its fields.
- Running `GET_HARDWARE_STATS` requests are serialized at an instruction boundary on the emulation thread and do not pause or otherwise change execution state.
- The protocol owner assigned IDs 96, 97, and 98 to the three new named commands. Legacy command 45 remains unchanged.
- A process-wide atomic counter allocates monotonic session IDs. The first externally visible ID is not guaranteed to be 1 because `Hardware` creates an initial internal session before the first TCP client connects.
- A statistics session begins immediately after the server accepts a TCP client. Session reset is serialized through the hardware thread using the internal, non-wire `INTERNAL_BEGIN_SESSION` request with value `-6`. This request is not advertised by `GET_SERVER_INFO` and is rejected by public IPC validation.
- Monotonic cycle and frame offsets compensate for the core CPU and display counters being reset by `RESET` and `RESTART`.
- Dismount preserves the in-memory disk bytes and dirty flag while clearing `mounted` and `path`. Dismounting the selected drive also clears the controller's active media pointer without changing its selected-drive register.

Two response fields currently differ from the intended schema and require an explicit compatibility decision:

- `displayMode` is currently serialized as a MessagePack Boolean because the core accessor returns `bool`, while the schema example specifies numeric `0` or `1`.
- `rusLat` is currently derived from bit 3 of the RUS/LAT history register, matching legacy statistics behavior, rather than directly from the current RUS/LAT latch.

Clients should not depend on those two implementation differences until schema 1 defines their final representation and source.

## 10. Acceptance Criteria

1. Stopped and running snapshots return all schema 1 fields, exactly 16 raw palette bytes, and exactly four drive entries.
2. Running snapshot requests succeed without stopping execution; both mutations remain rejected while hardware is running.
3. `cpuCycles` and `frameNumber` do not move backward across `RESET` or `RESTART` in one session.
4. A new connection changes `sessionId` and resets session counters.
5. `lastRunCycles` is positive after a non-empty run and remains stable across repeated stopped reads.
6. Palette indices and colors outside their ranges, missing fields, and unknown fields return `invalid_request` without mutation.
7. A successful palette write is immediately visible as the same raw byte in the next snapshot.
8. FDD indices outside 0 through 3 and unknown fields return `invalid_request` without mutation.
9. Dismount is idempotent, clears mounted state and path, preserves dirty data, and does not change the selected-drive register.
10. `GET_SERVER_INFO` advertises commands 96 through 98 and the schema/capability fields above.
