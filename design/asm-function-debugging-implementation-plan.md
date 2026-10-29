# Assembly Function Debugging Implementation Plan

**Status:** Initial implementation complete across v6asm, v6emul, and v6vscode;
extended acceptance checklist remains below.
**Date:** 2026-10-24
**Design:** `C:/Work/Programming/v6vscode/design/features/asm-function-debugging-options.md`

## Implementation checkpoint

Implemented producer contract: DWARF v4 region tag `0x4080` with address/size and
standard entry-point children, carrying name/address, source-path attribute
`0x3f01`, and declaration line. Object addresses use owning-section relocations;
the extension filters `0xFFFF` starts/entries. Real linked GC fixtures verify it.

Implemented backend contract: commands 111/112 with `functionHistorySchema: 1`,
one caller cursor, independent generation validation, and recorded-opcode
sequential successors. Region queries use 16-bit CPU coordinates, projecting
stored global instruction addresses to their low 16 bits.

Implemented extension: metadata indexing and definition declarations, raw caller
reconstruction and loop collapse, explicit history frames, per-stop capture
coalescing, macro frame presentation, and function Step Out through temporary
breakpoints. Historical caller registers are unavailable. Reconstruction has a
4096-occurrence safety bound.

Project-owned tests cover producer regions/relocations and ROM pruning, backend
IPC/history/generation behavior, linker tombstones, decoder/reduction behavior,
and adapter successor targeting. A real Extension Host/emulator test verifies
helper removal, source navigation, and function Step Out.

The phase checklists are the full acceptance roadmap, not a claim that every
edge-case test below has been implemented. Remaining extended coverage includes
multi-object external-entry fixtures, full-buffer wrap tests, same-block internal
call and indirect-transfer live scenarios, and never-hit tail-target live cleanup.

## 1. Outcome

Implement assembly function Call Stack and function Step Out using optional-block
metadata and executed-instruction history. Preserve existing breakpoints, Step
In, macro stepping, and C debugging.

A **function region** is one retained optional block. Every global source label
inside it is a **potential entry**. The observed entrance selects the frame name.
One region produces one function frame; internal calls stay within that frame.

A **history cursor** is an offset from the newest recorded instruction. A
**history generation** validates that the buffer has remained unchanged. A
**tombstone** is the linker marker for an address belonging to discarded code.

## 2. Ownership

| Project | Owned changes | Deliverable |
|---|---|---|
| v6asm | Capture optional-block boundaries and global labels; emit region/entry metadata and owning-section relocations; assembler fixtures and docs | Debug ELF containing discoverable regions and entries in object and direct-ROM modes |
| v6emul | Raw-history range search, one caller cursor, independent generation validation, sequential-successor query, protocol validation/capabilities, backend tests/docs | Metadata-independent query API using the current recording buffer |
| v6vscode | Decode/filter metadata, consume emulator API, reconstruct/collapse caller chain, present logical frames, implement function Step Out, integration tests/docs | Working native Call Stack navigation and function Step Out |
| v6clang / V6C linker | Verify existing relocation and discarded-address behavior with linked fixtures | Evidence that the existing linker supplies the required addresses/tombstones |

No linker implementation change is planned. If linked fixtures expose a target
relocation defect, record and fix that defect in v6clang before enabling affected
metadata. Physical removal of discarded DWARF records is outside scope.

Project roots in this workspace:

- `C:/Work/Programming/v6asm`
- `C:/Work/Programming/v6emul`
- `C:/Work/Programming/v6vscode`
- `C:/Work/Programming/v6clang`

All new filenames below are proposed; existing paths identify inspected code.

### Test ownership and storage

Tests that verify functionality implemented by a project must be stored and run
in that project's repository, using its existing test infrastructure. Commit the
test sources, required fixtures, and reproduction instructions with the owning
implementation.

| Functionality verified | Test repository |
|---|---|
| Optional-block capture, entry collection, DWARF emission, assembler relocations, and ROM output | v6asm |
| History scans, cursor/generation handling, successor computation, and IPC validation | v6emul |
| V6C linker garbage collection, relocation resolution, and tombstone behavior; regression tests for any linker fix | v6clang |
| Metadata decoding/filtering, protocol client, chain reduction, adapter presentation, and Step Out orchestration | v6vscode |
| Full user-visible Extension Host/emulator workflow | v6vscode end-to-end suite |

Cross-project acceptance tests complement each producer/backend's own tests.
An extension integration test does not replace assembler, emulator, or linker
coverage in the repository implementing that functionality. Consumer fixtures may
be copied or regenerated in v6vscode for decoding and workflow tests; record their
producer revisions and build commands.

## 3. Phase 0 — Freeze Producer/Consumer Contracts

**Owners:** v6asm + v6vscode for metadata; v6emul + v6vscode for requests.

### Metadata contract

- [ ] Agree the precise DWARF v4 representation of region identity, whole-block
  start/size, and entry name/address/definition. Record tags, attributes, forms,
  and any vendor identifiers in producer and consumer docs before coding.
- [ ] Represent one region independently of its several entry names. Preserve
  macro inline descriptions alongside region metadata.
- [ ] Use section-relative relocations for address fields in object output and
  final absolute addresses in direct-ROM companions.
- [ ] Define `0xFFFF` filtering for region starts and entry addresses. Filter starts
  before calculating range ends. Preserve ordinary sizes and exclusive ends.
- [ ] Specify `[start, end)` arithmetic with an exclusive end up to `0x10000`.
  Global here means a non-local source label, including ELF-local symbols.
- [ ] Produce a small annotated metadata fixture as the decoder contract.

### Emulator contract

Boundary query, with proposed field names:

```text
request:
    rangeStart, rangeEnd
    callerCursor?          absent for the initial live-PC search
    historyGeneration?     required with callerCursor

response:
    status                 found | historyEnd
    historyGeneration
    callerPc               present for found
    entrancePc             present for found
    callerCursor           present for found
```

- [ ] Initial search uses the live stopped PC as destination and scans newest
  history first. Continuations use the recorded PC at the supplied cursor as
  destination and scan strictly older items.
- [ ] Agree one address coordinate system for request ranges and returned PCs;
  v6emul owns conversion from its stored global instruction addresses.
- [ ] Define structured errors for running hardware, stale generations, invalid
  ranges, invalid cursors, and incomplete continuation parameters.
- [ ] Define a separate sequential-successor request taking `callerCursor` and
  `historyGeneration`, returning the caller's recorded PC plus opcode length,
  with 16-bit PC arithmetic. This uses the executed opcode even if memory changed.
- [ ] Allocate request IDs and capability/schema fields in both projects together.
  Encode generation values within JavaScript's safe integer range or use strings
  consistently in both endpoints.

**Gate:** Documented byte-level metadata and request/response examples accepted by
both sides. The sequential-successor request is distinct from a branch target.

## 4. Phase 1 — v6asm Region and Entry Metadata

**Owner:** v6asm. Depends on the metadata portion of Phase 0.

Primary existing files:

- `C:/Work/Programming/v6asm/crates/v6_core/src/assembler.rs`
- `C:/Work/Programming/v6asm/crates/v6_core/src/object/dwarf.rs`
- `C:/Work/Programming/v6asm/crates/v6_core/src/object/elf.rs`
- `C:/Work/Programming/v6asm/crates/v6_core/tests/object_tests.rs`
- `C:/Work/Programming/v6asm/docs/debug-metadata.md`

Tasks:

- [ ] Capture retained optional-block start/end and section identity during final
  assembly emission. Reset transient captures between passes and assemblies.
- [ ] Capture every global label's source name, block identity, address, and
  definition location, including labels reached only through another object or
  indirect execution. Entry collection is independent of section naming.
- [ ] Preserve block identity with multiple entries and shared bodies. For nested
  optional constructs, record actual emitted ownership and test the design's
  widest-containing-range selection rather than assuming source nesting implies
  contiguous linked addresses.
- [ ] Emit the agreed DWARF representation and owning-section relocations for all
  emitted blocks. Keep existing macro and instruction-line metadata intact.
- [ ] Emit final absolute regions/entries for direct-ROM companions after pruning
  and layout. Preserve current optional settings and pruning behavior.
- [ ] Add object tests for multiple labels, ELF-local labels, absent local
  references, relocation targets/addends, and unchanged code bytes.
- [ ] Add ROM tests for retained/pruned blocks, final addresses, and several entries.
- [ ] Document the emitted contract and regenerate reproducible fixture artifacts.

**Gate:** Inspect emitted objects/ELFs with the toolchain's `llvm-readelf` and
`llvm-dwarfdump`; all assembler tests pass.

## 5. Phase 2 — v6emul History and Successor Queries

**Owner:** v6emul. Can run in parallel with Phase 1 after protocol agreement.

Primary existing files:

- `C:/Work/Programming/v6emul/libs/v6core/include/core/trace_log.h`
- `C:/Work/Programming/v6emul/libs/v6core/src/trace_log.cpp`
- `C:/Work/Programming/v6emul/libs/v6core/include/core/hardware_consts.h`
- `C:/Work/Programming/v6emul/libs/v6core/src/debugger.cpp`
- `C:/Work/Programming/v6emul/libs/v6core/src/hardware.cpp`
- `C:/Work/Programming/v6emul/app/ipc_request.cpp`
- `C:/Work/Programming/v6emul/tests/integration_tests.cpp`
- `C:/Work/Programming/v6emul/tests/ipc_tests.cpp`
- `C:/Work/Programming/v6emul/docs/ipc-protocol.md`

Tasks:

- [ ] Add history-generation state independent of `m_filterGeneration` and
  `m_activeFilterId`. Invalidate it on buffer mutation/reset/replacement.
- [ ] Centralize cursor-to-buffer indexing and valid-history bounds. Check empty,
  partially filled, full, wrapped, and reset buffers; add a valid-count field if
  needed to prevent traversing stale slots after reset.
- [ ] Implement the boundary scan directly over raw records. Maintain one rolling
  destination address and return one outside cursor. Keep work bounded by retained
  buffer size and skip display formatting/filter allocation.
- [ ] Use live stopped PC only to initialize the first search. Recording behavior
  remains unchanged; a breakpoint at the first entrance works immediately.
- [ ] Implement sequential-successor lookup from the recorded opcode at the caller
  cursor using `CpuI8080::GetInstrLen`.
- [ ] Route both requests through paused hardware-thread handling; validate inputs
  and advertise capability/schema support through server information.
- [ ] Keep Trace Log filter creation/replacement independent of history-generation
  validity. Verify existing display queries continue working.
- [ ] Test transfers, repeated addresses, strict older continuation, history end,
  stale tokens, first-instruction breakpoints, and successor arithmetic.
- [ ] Add API documentation with initial/continuation/successor examples.

**Existing-code constraint:** `Hardware::GetStepOverAddr()` uses the current live
PC and can return taken branch destinations. It is not the historical caller's
sequential-successor operation. Reuse opcode-length logic, not that behavior.

**Gate:** Backend integration and IPC validation tests pass against the actual
request handlers. Filtering while paused leaves caller cursors valid.

## 6. Phase 3 — Linked Metadata Verification and Extension Decoder

**Owners:** v6asm for emitted artifacts and producer tests; v6clang for linker
behavior tests and any linker defects; v6vscode for decoder and consumer tests.
Depends on Phase 1.

v6vscode implementation areas:

- `C:/Work/Programming/v6vscode/src/debug/metadata/dwarf-reader.ts`
- `C:/Work/Programming/v6vscode/src/debug/metadata/dwarf-scopes.ts`
- `C:/Work/Programming/v6vscode/src/debug/metadata/debug-metadata-index.ts`
- `C:/Work/Programming/v6vscode/src/debug/metadata/debug-artifact-loader.ts`
- `C:/Work/Programming/v6vscode/test/fixtures/asmdbg`
- `C:/Work/Programming/v6vscode/test/unit/debug`

Tasks:

- [ ] Create a two-object fixture: caller references an optional entry defined in
  the other object; include another unused optional block and an indirect entry.
- [ ] Link with and without `--gc-sections`. Verify relocation results for live
  entries and tombstones for discarded addresses using the installed V6C linker.
- [ ] Store producer fixture tests in v6asm, linker behavior/regression tests in
  v6clang, and decoder fixture tests in v6vscode according to the ownership table.
- [ ] Check the existing V6C tombstone relocation path in
  `C:/Work/Programming/v6clang/lld/ELF/InputSection.cpp` only if results disagree.
- [ ] Decode region identity and all potential entries into an immutable index.
  Expose widest-region-at-PC and entry-at-entrance queries separate from existing
  C physical-subprogram lookup.
- [ ] Filter tombstoned starts/entries before constructing ranges; test nonzero
  sizes, address wrap, legitimate exclusive ends, and the accepted `0xFFFF` loss.
- [ ] Add declared feature detection so older ELF artifacts retain current behavior.
- [ ] Add direct-ROM and linked-ELF decoder tests, including existing macro chains
  and entry definition paths.

**Gate:** Real linked artifacts, not just synthetic buffers, demonstrate that
discarded code never appears in extension region/entry lookup indexes.

## 7. Phase 4 — Extension History Client and Chain Reconstruction

**Owner:** v6vscode. Depends on Phases 2 and 3; unit work can use protocol fixtures.

Existing protocol integration:

- `C:/Work/Programming/v6vscode/src/emulator/protocol/ipc-commands.ts`
- `C:/Work/Programming/v6vscode/src/emulator/protocol/ipc-server-info.ts`

Proposed new services:

- `C:/Work/Programming/v6vscode/src/debug/session/function-history-client.ts`
- `C:/Work/Programming/v6vscode/src/debug/adapter/function-call-chain-service.ts`

Tasks:

- [ ] Add typed requests, strict response validation, and capability checks.
- [ ] Cache one reconstruction per stopped generation and coalesce overlapping
  requests. Discard in-flight results after resume/reset/reload/disconnect.
- [ ] Query the widest containing region, resolve the observed entrance label,
  then continue with the returned caller cursor and unchanged generation.
- [ ] Stop at unwrapped code or `historyEnd`; retain terminal unwrapped call-site
  context so Step Out can use it.
- [ ] Gather raw occurrences and collapse every repeated-region loop. Preserve
  the newer child-transfer site and the older entry/parent provenance when merging.
- [ ] Keep calls entirely inside a region in the existing region frame. Entry
  name stays fixed while source location follows actual execution.
- [ ] Test nested/repeated helper excursions, jump-only loops, shared entries,
  terminal callers, exhausted history, and stale-generation rejection.
- [ ] Measure query count and scan work on retained history; apply cancellation
  and a bounded reconstruction policy consistent with existing session handling.

**Gate:** Deterministic unit traces produce the agreed chain and navigation sites,
including `main -> A -> B -> A -> C` reducing to `C -> A -> main`.

## 8. Phase 5 — Call Stack Integration and Function Step Out

**Owner:** v6vscode. Depends on Phase 4.

Existing integration points:

- `C:/Work/Programming/v6vscode/src/debug/adapter/stack-trace-service.ts`
- `C:/Work/Programming/v6vscode/src/debug/adapter/v6-debug-adapter.ts`
- `C:/Work/Programming/v6vscode/src/debug/adapter/scope-service.ts`
- `C:/Work/Programming/v6vscode/src/debug/adapter/variable-service.ts`
- `C:/Work/Programming/v6vscode/src/debug/session/breakpoint-coordinator.ts`
- `C:/Work/Programming/v6vscode/src/language/symbols/symbol-link-service.ts`

Tasks:

- [ ] Extend the frame model to distinguish physical frames from history frames.
  Existing `DapFrameContext` requires `PhysicalFrame`; update consumers explicitly
  rather than constructing fake saved-register frames.
- [ ] Use history frames for the new assembly regions and retain existing C
  unwinding behavior. Retain existing fallback when metadata/API support is absent.
- [ ] Expand existing macro inline contexts around each logical function site.
  Preserve generation-bound IDs, paging, and capture invalidation.
- [ ] Show active body source and caller transfer sites; resolve Go to Definition
  through entry declarations. Preserve live CPU registers; historical registers
  remain unavailable unless independently recoverable.
- [ ] For function Step Out, obtain the reconstructed caller cursor/generation,
  request its sequential successor while paused, acquire the temporary breakpoint,
  then resume. Check address presence explicitly so address zero is valid.
- [ ] Preserve macro Step Out precedence and existing C physical Step Out.
- [ ] Reuse temporary-breakpoint ownership/cleanup on user stop, cancellation,
  reset, and disconnect. A target never reached remains pending during execution
  under the accepted tail-transfer limitation.
- [ ] Keep breakpoints and Step In unchanged; run regression tests for them.

**Gate:** Adapter tests validate correct frame selection/navigation, successor
request, temporary breakpoint target, cleanup, and stale-response cancellation.

## 9. Phase 6 — End-to-End Acceptance and Release

**Owner:** v6vscode leads acceptance; v6asm and v6emul supply built tools and tests.

Each project completes and runs its repository-owned tests before this combined
acceptance phase. The tests below verify integration and user-visible behavior.

- [ ] Add reproducible assembly fixture source/build instructions under
  `C:/Work/Programming/v6vscode/test/fixtures/asmdbg`.
- [ ] Add real Extension Host/emulator tests under
  `C:/Work/Programming/v6vscode/src/test/integration/suite` using current harnesses.
- [ ] Verify object-linked and direct-ROM artifacts, multi-entry naming, completed
  helper removal, same-block omitted calls, nested macros, and body breakpoints.
- [ ] Verify first-entry breakpoint, terminal unwrapped caller, actual caller-site
  navigation, live PC stability, and normal-call Step Out.
- [ ] Verify a tail jump uses the same successor breakpoint and can keep running;
  pause the test explicitly and assert temporary-breakpoint cleanup.
- [ ] Test new extension with old emulator/artifact and old supported features
  with new metadata/API. Log unsupported capability without inventing callers.
- [ ] Update each project's own metadata/protocol/user documentation and record
  tested tool revisions and schema versions in fixture instructions.

### Validation commands

Run commands from their owning repository. CMake presets require the documented
local compiler/dependencies; use the existing configured preset where appropriate.

| Owner | Commands |
|---|---|
| v6asm | `cargo test -p v6_core --test object_tests`; `cargo test --workspace` |
| v6emul | `cmake --preset debug`; `cmake --build --preset debug`; `ctest --test-dir build/debug --build-config Debug --output-on-failure` |
| v6vscode | `npm run compile`; `npm run lint`; `npm run test:core`; `npm run test:all`; `npm run test:integration` |
| Linked fixtures | Toolchain `llvm-readelf -S -r` and `llvm-dwarfdump --debug-info --debug-line` on objects and final ELFs; inspect GC and non-GC outputs |

These commands define the acceptance suites. The implementation checkpoint above
distinguishes delivered coverage from remaining extended scenarios.

## 10. Delivery Order and Commit Boundaries

1. Freeze metadata and protocol contracts (Phase 0).
2. Land v6asm producer and v6emul API independently (Phases 1–2).
3. Verify linked output and land extension decoding (Phase 3).
4. Land extension history client/reduction, then UI and Step Out (Phases 4–5).
5. Run cross-project acceptance and publish compatible tool builds (Phase 6).

Keep commits scoped to their owning repository. Suggested subjects:

- v6asm: `feat: emit optional-block function region and entry metadata`
- v6emul: `feat: query caller boundaries and sequential successors from history`
- v6vscode: `feat: decode assembly function regions and filter tombstones`
- v6vscode: `feat: reconstruct assembly caller chains from execution history`
- v6vscode: `feat: integrate assembly call stacks and function step out`
- v6vscode: `test: verify assembly function debugging across toolchain outputs`