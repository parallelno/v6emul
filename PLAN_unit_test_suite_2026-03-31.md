# Unit Test Suite Implementation Plan

**Date:** 2026-03-31
**Scope:** Automated assembly-level unit testing for the i8080 CPU emulator
**Test directory:** `tests/unit_tests/` (33 ASM files)

---

## Overview

Build a Python-based test runner that assembles each `.asm` file from `tests/unit_tests/`, runs the resulting ROM through the emulator, captures the CPU register dump, and validates against expected values. Results are printed per-test with a final summary.

---

## Phase 1 — Prepare ASM Unit Tests

### 1.1 Add `DI` after `.org 0x100`

Every test must begin with `DI` (disable interrupts) immediately after the `.org 0x100` directive. This prevents the emulator's interrupt handler from corrupting registers or branching away during the test.

**Before:**
```asm
.org 0x100
    MVI A, 0x42
```

**After:**
```asm
.org 0x100
    DI
    MVI A, 0x42
```

### 1.2 Add `HLT` at the end of each logic branch

Every execution path must terminate with `HLT` so the emulator stops cleanly with `--halt-exit`. Tests with branches need `HLT` on **each** reachable path endpoint.

**Simple test (single path):**
```asm
.org 0x100
    DI
    MVI A, 0x42
    HLT
```

**Branching test (multiple paths):**
```asm
.org 0x100
    DI
    MVI A, 0x01
    JNZ target
    MVI A, 0xAA
    HLT             ; branch: JNZ not taken
target:
    MVI A, 0xCC
    HLT             ; branch: JNZ taken
```

### 1.3 Per-file changes

| File | Changes needed |
|------|---------------|
| `adc_carry.asm` | Add `DI` after `.org`, add `HLT` at end |
| `add_b.asm` | Add `DI` after `.org`, add `HLT` at end |
| `add_carry.asm` | Add `DI` after `.org`, add `HLT` at end |
| `ana_b.asm` | Add `DI` after `.org`, add `HLT` at end |
| `call_ret.asm` | Add `DI` after `.org`, add `HLT` after `done: NOP` |
| `cma.asm` | Add `DI` after `.org`, add `HLT` at end |
| `cmc.asm` | Add `DI` after `.org`, add `HLT` at end |
| `cmp_b.asm` | Add `DI` after `.org`, add `HLT` at end |
| `dad_b.asm` | Add `DI` after `.org`, add `HLT` at end |
| `dcr_b.asm` | Add `DI` after `.org`, add `HLT` at end |
| `dcx_h.asm` | Add `DI` after `.org`, add `HLT` at end |
| `inr_b.asm` | Add `DI` after `.org`, add `HLT` at end |
| `inx_h.asm` | Add `DI` after `.org`, add `HLT` at end |
| `jmp.asm` | Add `DI` after `.org`, add `HLT` after `skip: MVI A, 0xBB` |
| `jnz.asm` | Add `DI` after `.org`, add `HLT` after `MVI A, 0xAA` (not-taken), add `HLT` after `target: MVI A, 0xCC` (taken) |
| `jz_not_taken.asm` | Add `DI` after `.org`, add `HLT` after `MVI A, 0xDD`, add `HLT` after `skip: NOP` |
| `lda.asm` | Add `DI` after `.org`, add `HLT` at end |
| `lxi_h.asm` | Add `DI` after `.org`, add `HLT` at end |
| `mov_a_b.asm` | Add `DI` after `.org`, add `HLT` at end |
| `mov_m_r.asm` | Add `DI` after `.org`, add `HLT` at end |
| `mvi_a.asm` | Add `DI` after `.org`, add `HLT` at end |
| `ora_b.asm` | Add `DI` after `.org`, add `HLT` at end |
| `push_pop.asm` | Add `DI` after `.org`, add `HLT` at end |
| `rlc.asm` | Add `DI` after `.org`, add `HLT` at end |
| `rrc.asm` | Add `DI` after `.org`, add `HLT` at end |
| `sbb_carry.asm` | Add `DI` after `.org`, add `HLT` at end |
| `sta.asm` | Add `DI` after `.org`, add `HLT` at end |
| `stc.asm` | Add `DI` after `.org`, add `HLT` at end |
| `sub_b.asm` | Add `DI` after `.org`, add `HLT` at end |
| `sub_zero.asm` | Add `DI` after `.org`, add `HLT` at end |
| `xra_a.asm` | Add `DI` after `.org`, add `HLT` at end |
| `xra_b.asm` | Add `DI` after `.org`, add `HLT` at end |

---

## Phase 2 — Expected Results Definition

### 2.1 Expected register values file

Create `tests/unit_tests/expected.json` — a JSON file mapping each test name to the expected CPU register values after execution. Only registers relevant to the test need to be specified; unspecified registers are not checked.

**Flags register (F) bit layout (i8080):**

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|-----|---|---|---|---|---|---|---|---|
| Flag | S | Z | 0 | AC | 0 | P | 1 | CY |

**Format:**
```json
{
    "mvi_a": { "A": "42" },
    "add_b": { "A": "33" },
    "sub_b": { "A": "05" },
    ...
}
```

### 2.2 Expected values per test

Below are the predicted register values (hex, uppercase) for each test after execution with `DI` + `HLT` added. F is the full flags byte.

| Test | A | F | B | C | D | E | H | L | Notes |
|------|---|---|---|---|---|---|---|---|-------|
| `adc_carry` | 11 | 06 | 10 | — | — | — | — | — | 0x80+0x80=0x00 (CY), 0x00+0x10+1=0x11, F=00000110 (P=1,bit1=1) |
| `add_b` | 33 | 06 | 22 | — | — | — | — | — | 0x11+0x22=0x33, F=00000110 |
| `add_carry` | 00 | 57 | 80 | — | — | — | — | — | 0x80+0x80=0x00, F=01010111 (S=0,Z=1,AC=1,P=1,CY=1) |
| `ana_b` | 10 | 06 | 30 | — | — | — | — | — | 0x1F & 0x30 = 0x10 |
| `call_ret` | EE | — | — | — | — | — | — | — | Subroutine sets A=0xEE |
| `cma` | 0F | — | — | — | — | — | — | — | ~0xF0 = 0x0F |
| `cmc` | 00 | — | — | — | — | — | — | — | Carry cleared; check CY=0 in F |
| `cmp_b` | 20 | — | 10 | — | — | — | — | — | CMP doesn't modify A |
| `dad_b` | — | — | 11 | 11 | — | — | 22 | 22 | 0x1111+0x1111=0x2222 |
| `dcr_b` | — | — | 0E | — | — | — | — | — | 0x0F-1=0x0E |
| `dcx_h` | — | — | — | — | — | — | 0F | FF | 0x1000-1=0x0FFF |
| `inr_b` | — | — | 10 | — | — | — | — | — | 0x0F+1=0x10 |
| `inx_h` | — | — | — | — | — | — | 10 | 00 | 0x0FFF+1=0x1000 |
| `jmp` | BB | — | — | — | — | — | — | — | Skips 0xAA, loads 0xBB |
| `jnz` | CC | — | — | — | — | — | — | — | Jumps (Z=0), A=0xCC |
| `jz_not_taken` | DD | — | — | — | — | — | — | — | Doesn't jump (Z=0), A=0xDD |
| `lda` | 99 | — | — | — | — | — | — | — | Store then load 0x99 |
| `lxi_h` | — | — | — | — | — | — | 12 | 34 | HL=0x1234 |
| `mov_a_b` | 55 | — | 55 | — | — | — | — | — | B=0x55, MOV A,B |
| `mov_m_r` | 88 | — | — | — | — | — | 02 | 00 | HL=0x0200, A=0x88 |
| `mvi_a` | 42 | — | — | — | — | — | — | — | A=0x42 |
| `ora_b` | 3F | — | 30 | — | — | — | — | — | 0x0F | 0x30 = 0x3F |
| `push_pop` | — | — | AB | CD | — | — | — | — | BC=0xABCD after push/pop |
| `rlc` | 05 | — | — | — | — | — | — | — | 0x82 rotate left = 0x05, CY=1 |
| `rrc` | C1 | — | — | — | — | — | — | — | 0x83 rotate right = 0xC1, CY=1 |
| `sbb_carry` | 0E | — | F0 | — | — | — | — | — | 0xFF-0xF0-1=0x0E |
| `sta` | 77 | — | — | — | — | — | — | — | A=0x77 |
| `stc` | — | — | — | — | — | — | — | — | Carry=1; check F bit 0 |
| `sub_b` | 05 | — | 0B | — | — | — | — | — | 0x10-0x0B=0x05 |
| `sub_zero` | 00 | — | 42 | — | — | — | — | — | 0x42-0x42=0x00, Z=1 |
| `xra_a` | 00 | — | — | — | — | — | — | — | 0xFF XOR 0xFF = 0x00, Z=1 |
| `xra_b` | 2F | — | 30 | — | — | — | — | — | 0x1F XOR 0x30 = 0x2F |

> **Note:** "—" means the register is not checked for that test. The `F` values above are approximate estimates. During implementation, run each test once to capture the actual emulator output, then lock the expected values in `expected.json`. This is the "golden capture" step.

---

## Phase 3 — Test Runner Implementation

### 3.1 Technology choice: **Python script**

**Rationale:**
- Portable (runs on Windows, Linux, macOS)
- Easy subprocess management and stdout parsing
- JSON parsing built-in
- Already common for test orchestration
- No additional build step needed

### 3.2 File: `tests/run_unit_tests.py`

**Responsibilities:**
1. Discover all `.asm` files in `tests/unit_tests/`
2. Assemble each file with `v6asm` into `out/tests/unit_tests/<name>.rom`
3. Run each ROM with the emulator and capture stdout
4. Parse the CPU register dump from stdout
5. Compare actual registers against `expected.json`
6. Print per-test PASS/FAIL report
7. Print final summary statistics

**CLI interface:**
```
python tests/run_unit_tests.py [options]

Options:
  --asm <path>        Path to v6asm.exe        (default: tools/v6asm/v6asm.exe)
  --emu <path>        Path to v6emul.exe       (default: build/release/app/Release/v6emul.exe)
  --test-dir <path>   Path to unit_tests/      (default: tests/unit_tests)
  --out-dir <path>    Output directory for ROMs (default: out/tests/unit_tests)
  --expected <path>   Path to expected.json     (default: tests/unit_tests/expected.json)
  --filter <pattern>  Run only tests matching glob pattern (e.g. "add*")
  --verbose           Show full emulator output for each test
  --capture           Run tests and dump actual register values (for creating/updating expected.json)
```

### 3.3 Script structure (pseudocode)

```python
def main():
    args = parse_args()
    expected = load_json(args.expected)
    tests = discover_asm_files(args.test_dir)
    os.makedirs(args.out_dir, exist_ok=True)

    results = []
    for asm_file in sorted(tests):
        name = stem(asm_file)

        # Step 1: Assemble
        rom_path = out_dir / f"{name}.rom"
        ok, err = run_assembler(args.asm, asm_file, rom_path)
        if not ok:
            results.append(Result(name, "ASM_FAIL", err))
            continue

        # Step 2: Run emulator
        ok, stdout = run_emulator(args.emu, rom_path)
        if not ok:
            results.append(Result(name, "RUN_FAIL", stdout))
            continue

        # Step 3: Parse registers
        actual_regs = parse_cpu_dump(stdout)
        if actual_regs is None:
            results.append(Result(name, "PARSE_FAIL", stdout))
            continue

        # Step 4: Validate
        if name not in expected:
            results.append(Result(name, "NO_EXPECTED", actual_regs))
            continue

        mismatches = compare(expected[name], actual_regs)
        if mismatches:
            results.append(Result(name, "FAIL", mismatches))
        else:
            results.append(Result(name, "PASS"))

    # Step 5: Report
    print_report(results)
    print_summary(results)
    sys.exit(0 if all_passed(results) else 1)
```

### 3.4 Emulator invocation

```
v6emul.exe -rom <file.rom> -load-addr 0x100 --halt-exit --dump-cpu --run-cycles 100000
```

### 3.5 Output parsing

The emulator produces two lines on exit:
```
HALT at PC=0x0105 after 27 cpu_cycles 0 frames
CPU: A=42 F=02 B=00 C=00 D=00 E=00 H=00 L=00
```

Or when timeout (no HLT reached):
```
EXIT at PC=0x61A8 after 100000 cpu_cycles 1 frames
CPU: A=42 F=02 B=00 C=00 D=00 E=00 H=00 L=00
```

Parse with regex:
```python
# Line 1: halt/exit status
re.match(r'(HALT|EXIT) at PC=0x([0-9A-Fa-f]+) after (\d+) cpu_cycles', line1)

# Line 2: register values
re.match(r'CPU: A=([0-9A-Fa-f]{2}) F=([0-9A-Fa-f]{2}) '
         r'B=([0-9A-Fa-f]{2}) C=([0-9A-Fa-f]{2}) '
         r'D=([0-9A-Fa-f]{2}) E=([0-9A-Fa-f]{2}) '
         r'H=([0-9A-Fa-f]{2}) L=([0-9A-Fa-f]{2})', line2)
```

A test that exits with `EXIT` (timeout) instead of `HALT` should be flagged as a warning — the test didn't reach a `HLT` instruction.

### 3.6 Report format

```
=== Unit Test Results ===

[PASS] add_b          A=33 F=06 B=22 C=00
[PASS] mvi_a          A=42
[FAIL] sub_b          A: expected=05 actual=06
[ASM_FAIL] bad_test   Assembler error: line 3: unknown mnemonic 'FOO'
[WARN] no_halt        Exited by cycle limit (no HLT reached)

=== Summary ===
Total: 33  Passed: 31  Failed: 1  Errors: 1
```

---

## Phase 4 — CMake Integration

### 4.1 Add CTest target

Add to `tests/CMakeLists.txt`:

```cmake
# ASM unit tests (assemble + run + validate via Python)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME asm_unit_tests
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_unit_tests.py
        --asm ${CMAKE_SOURCE_DIR}/tools/v6asm/v6asm.exe
        --emu $<TARGET_FILE:v6emul>
        --test-dir ${CMAKE_CURRENT_SOURCE_DIR}/unit_tests
        --out-dir ${CMAKE_BINARY_DIR}/tests/unit_tests_out
        --expected ${CMAKE_CURRENT_SOURCE_DIR}/unit_tests/expected.json
)
```

This allows running `ctest --test-dir build/release -R asm_unit_tests` as part of CI.

### 4.2 CI integration

The existing GitHub Actions CI workflow already runs `ctest`. Once the `asm_unit_tests` test is registered, it will automatically run in CI on all platforms (assuming Python 3 is available on CI runners — it is on `windows-latest`, `ubuntu-latest`, `macos-latest`).

---

## Phase 5 — Golden Capture (Bootstrap)

### 5.1 Initial capture

After modifying all `.asm` files (Phase 1) and building the test runner (Phase 3):

```bash
python tests/run_unit_tests.py --capture > tests/unit_tests/captured.json
```

### 5.2 Review and lock

1. Manually review each captured register value against the test's expected behavior (use the table in Phase 2.2 as reference).
2. Copy verified values into `expected.json`.
3. Run the suite to confirm all tests pass: `python tests/run_unit_tests.py`

---

## Implementation Order

| Step | Description | Files Modified/Created |
|------|-------------|----------------------|
| 1 | Add `DI` + `HLT` to all 33 `.asm` files | `tests/unit_tests/*.asm` |
| 2 | Create `expected.json` with initial expected values | `tests/unit_tests/expected.json` (new) |
| 3 | Implement `run_unit_tests.py` | `tests/run_unit_tests.py` (new) |
| 4 | Run golden capture, verify and lock expected values | `tests/unit_tests/expected.json` (updated) |
| 5 | Add CTest integration | `tests/CMakeLists.txt` |
| 6 | Verify full suite passes | — |

---

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Flags (F) are hard to predict exactly | Use `--capture` mode to get actual values, then lock them |
| Some instructions may not be supported by `v6asm` (e.g. CMA) | Already handled via `DB 0x2F` raw opcode in `cma.asm` |
| Emulator interrupt handler fires before `DI` executes | `.org 0x100` + `DI` as the very first instruction + `-load-addr 0x100` ensures PC starts at DI |
| v6asm not available on non-Windows CI | v6asm.exe is only for Windows; Linux/macOS CI would need a cross-compile or skip ASM tests |
| Test takes too long | `--run-cycles 100000` is the safety timeout; all tests finish in < 100 cycles |

---

## Future Extensions

- **Flag-specific assertions:** Check individual flag bits (S, Z, AC, P, CY) instead of the full F byte.
- **Memory assertions:** Validate memory contents at specific addresses (e.g., `sta.asm`, `mov_m_r.asm`).
- **Auto-generate tests:** Script to scaffold a new test `.asm` + expected entry from a template.
- **Cycle-count validation:** Assert that a test completes in an expected number of CPU cycles.
- **Coverage report:** Track which i8080 opcodes are covered by the test suite.
