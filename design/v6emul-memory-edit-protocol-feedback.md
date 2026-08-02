#### Problem:

The client must present one authoritative list of memory edits, keep it synchronized with the server, show the current byte, update an entered value or readonly state, restore the original byte, and distinguish deletion from deletion with restoration. It must do this for Main RAM and every RAM-disk bank without depending on server implementation details or composing request sequences that expose partial results.

#### Current solution:

The server currently exposes these requests:

- `DEBUG_MEMORY_EDIT_ADD`: accepts the legacy record fields `globalAddr`, `value`, `readonly`, `active`, and `comment`; address and value are formatted hexadecimal strings rather than numeric wire values.
- `DEBUG_MEMORY_EDIT_DEL_ALL`: deletes all records.
- `DEBUG_MEMORY_EDIT_DEL`: deletes the record at one supplied address.
- `DEBUG_MEMORY_EDIT_GET`: returns the record at one supplied address.
- `DEBUG_MEMORY_EDIT_EXISTS`: reports whether one supplied address has a record.
- `GET_MEM`: reads current bytes.
- `SET_BYTE_GLOBAL`: writes one current byte.

This is not enough for the panel because:

- The client cannot discover the full collection without already knowing every address.
- `GET_SERVER_INFO` does not advertise a memory-edit schema, so the client cannot distinguish legacy and supported contracts.
- The legacy names `value`, `readonly`, and `active` do not define the requested `enteredValue`, `readonly`, and `active` behavior precisely.
- There is no server request for restoring the original value.
- The interface does not state whether records survive reset/restart, program reload, reconnect, or a new server session.
- The interface does not define structured field errors for malformed addresses, values, booleans, or unknown fields.

#### Needed:

Use numeric wire values rather than formatted hexadecimal strings similarly as other request protocols, for example GET_STOP_RECORD

I recommend improvements:

- Separate the client-writable input from the server-returned snapshot:
  - `MemoryEditInput`: `{ globalAddr, enteredValue, readonly, active, comment }`.
  - `MemoryEditSnapshot`: `{ globalAddr, enteredValue, originalValue, currentValue, readonly, active, comment }`.
  - `originalValue` and `currentValue` are server-owned, read-only fields. Clients cannot supply or update them.
- The server captures `originalValue` from memory when a record is first added. Updating an existing record preserves its `originalValue`.
- add `DEBUG_MEMORY_EDIT_GET_ALL`
  - Request: no data.
  - Response: `{ edits: MemoryEditSnapshot[] }`, sorted by `globalAddr` with no duplicate addresses.
  - Each snapshot contains its own `currentValue`, sampled atomically with the collection on the emulation thread.
  - Keeping `currentValue` in its snapshot associates it unambiguously with the corresponding record; it is not returned in a separate, positionally matched collection.
- add `DEBUG_MEMORY_EDIT_RESTORE`
  - Request: `{ globalAddr }`.
  - Atomically writes `originalValue` to memory and deletes the record.
  - Response: `{ globalAddr, restoredValue, deleted: true }`.
  - If no record exists, return `invalid_request` with `details.command` and `details.field = "globalAddr"`.

The schema must also guarantee:

- `globalAddr` is an integer within advertised global-memory bounds, including RAM-disk addresses.
- `enteredValue`, `originalValue`, and `currentValue` are integers in `0..255`.
- Adding a record with `active` `On` immediately writes `enteredValue` to memory.
- Updating `enteredValue` while `active` is `On`, or changing `active` from `Off` to `On`, immediately writes `enteredValue` to memory.
- `active` `Off` retains the record but does not apply or enforce `enteredValue`. Turning it off does not restore `originalValue`.
- `readonly` `Off` applies `enteredValue` once when the active record is added or updated; subsequent memory writes may replace it.
- `readonly` `On` and `active` `On` reject subsequent emulated CPU writes to the edited byte. The record remains enforced for CPU execution until it is updated, deactivated, restored, or deleted.
- After loading a ROM, the server reapplies `enteredValue` for every active record. Active readonly records continue rejecting subsequent emulated CPU writes.
- Direct memory changes made through requests such as `SET_MEM` or `SET_BYTE_GLOBAL` are not intercepted and do not cause memory edits to be reapplied.
- Reset and restart do not cause memory edits to be reapplied. Records survive these operations, but the operations may change the current byte independently of the retained record.
- Deleting a record does not change memory. Restoring a record writes `originalValue` and deletes the record atomically.
- Records survive reset, restart, ROM loading, and TCP reconnect, and are cleared when the emulator process exits.
- Invalid requests return structured `invalid_request` errors with `details.command` and `details.field`.
- Unknown fields are rejected.

`GET_SERVER_INFO.capabilities` should advertise support and bounds, following the existing breakpoint and watchpoint capability pattern:

```json
{
  "memoryEditSchema": 1,
  "memoryEditLimits": {
    "globalAddressExclusive": 2162688,
    "maxCommentBytes": 1024
  }
}
```

The client uses `memoryEditSchema` to distinguish this contract from the legacy hexadecimal-string contract. It uses `globalAddressExclusive` to validate Main RAM and RAM-disk addresses without hard-coding the server's memory layout.

#### Implementation notes

The implementation follows the behavior above with these representational and compatibility details:

- `MemoryEditInput` and `MemoryEditSnapshot` are wire-protocol shapes, not separate C++ structs. The core stores one `MemoryEdit` containing `enteredValue` and the server-owned `originalValue`; `currentValue` is read from memory only when a snapshot is produced.
- The IPC schema uses numeric values, but the existing debug-file storage format remains hexadecimal for compatibility. Saved records now include `originalValue`. When loading an older record without that field, the server captures the current byte as its fallback original value before applying the edit.
- `DEBUG_MEMORY_EDIT_GET_ALL` and `DEBUG_MEMORY_EDIT_RESTORE` use command IDs `99` and `100` respectively.
- `DEBUG_MEMORY_EDIT_GET` returns a `MemoryEditSnapshot` for an existing record and JSON `null` when no record exists. This missing-record result was not specified in the original recommendation.
- ROM-load reapplication is implemented through the internal, non-IPC command `INTERNAL_REAPPLY_MEMORY_EDITS`. It runs after ROM data is loaded and restart completes, before optional autorun.
- Updating an active record reapplies `enteredValue` only when the entered value changes or the record transitions from inactive to active. Updating only `comment` or `readonly` does not rewrite the current byte.