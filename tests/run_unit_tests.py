#!/usr/bin/env python3
"""
Unit test runner for i8080 ASM tests.
Assembles .asm files, runs them through the emulator, and validates register values.
"""

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional


@dataclass
class TestResult:
    name: str
    status: str  # PASS, FAIL, ASM_FAIL, RUN_FAIL, PARSE_FAIL, NO_EXPECTED, WARN
    detail: str = ""
    actual_regs: Dict[str, str] = field(default_factory=dict)
    mismatches: Dict[str, tuple] = field(default_factory=dict)
    halted: bool = True


REG_NAMES = ["A", "F", "B", "C", "D", "E", "H", "L"]

CPU_RE = re.compile(
    r"CPU:\s+A=([0-9A-Fa-f]{2})\s+F=([0-9A-Fa-f]{2})\s+"
    r"B=([0-9A-Fa-f]{2})\s+C=([0-9A-Fa-f]{2})\s+"
    r"D=([0-9A-Fa-f]{2})\s+E=([0-9A-Fa-f]{2})\s+"
    r"H=([0-9A-Fa-f]{2})\s+L=([0-9A-Fa-f]{2})"
)

STATUS_RE = re.compile(r"(HALT|EXIT)\s+at\s+PC=0x([0-9A-Fa-f]+)\s+after\s+(\d+)\s+cpu_cycles")


def parse_args():
    p = argparse.ArgumentParser(description="i8080 ASM unit test runner")
    default_asm = Path("tools/v6asm") / ("v6asm.exe" if os.name == "nt" else "v6asm")
    p.add_argument("--asm", default=str(default_asm), help="Path to v6asm assembler")
    p.add_argument("--emu", default="build/release/app/Release/v6emul.exe", help="Path to v6emul emulator")
    p.add_argument("--test-dir", default="tests/unit_tests", help="Path to unit_tests directory")
    p.add_argument("--out-dir", default="out/tests/unit_tests", help="Output directory for ROMs")
    p.add_argument("--expected", default="tests/unit_tests/expected.json", help="Path to expected.json")
    p.add_argument("--filter", default=None, help="Run only tests matching glob pattern")
    p.add_argument("--verbose", action="store_true", help="Show full emulator output")
    p.add_argument("--capture", action="store_true", help="Dump actual register values as JSON")
    return p.parse_args()


def discover_tests(test_dir: str, pattern: Optional[str] = None) -> List[Path]:
    tests = sorted(Path(test_dir).glob("*.asm"))
    if pattern:
        tests = [t for t in tests if fnmatch.fnmatch(t.stem, pattern)]
    return tests


def run_assembler(asm_path: str, source: Path, output: Path) -> tuple:
    try:
        result = subprocess.run(
            [asm_path, str(source), "-o", str(output)],
            capture_output=True, text=True, timeout=30
        )
        if result.returncode != 0:
            return False, result.stderr.strip() or result.stdout.strip()
        return True, ""
    except FileNotFoundError:
        return False, f"Assembler not found: {asm_path}"
    except subprocess.TimeoutExpired:
        return False, "Assembler timed out"


def run_emulator(emu_path: str, rom_path: Path) -> tuple:
    try:
        result = subprocess.run(
            [emu_path, "--rom", str(rom_path), "--load-addr", "0x100",
             "--halt-exit", "--dump-cpu", "--run-cycles", "100000"],
            capture_output=True, text=True, timeout=30
        )
        stdout = result.stdout.strip()
        return True, stdout
    except FileNotFoundError:
        return False, f"Emulator not found: {emu_path}"
    except subprocess.TimeoutExpired:
        return False, "Emulator timed out"


def parse_cpu_dump(stdout: str) -> tuple:
    """Returns (regs_dict, halted) or (None, False) on parse failure."""
    lines = stdout.strip().splitlines()

    halted = True
    for line in lines:
        m = STATUS_RE.search(line)
        if m:
            halted = m.group(1) == "HALT"
            break

    for line in lines:
        m = CPU_RE.search(line)
        if m:
            regs = {}
            for i, name in enumerate(REG_NAMES):
                regs[name] = m.group(i + 1).upper()
            return regs, halted

    return None, False


def compare_regs(expected: Dict[str, str], actual: Dict[str, str]) -> Dict[str, tuple]:
    mismatches = {}
    for reg, exp_val in expected.items():
        exp_upper = exp_val.upper()
        act_val = actual.get(reg, "??")
        if act_val.upper() != exp_upper:
            mismatches[reg] = (exp_upper, act_val.upper())
    return mismatches


def run_test(name: str, asm_file: Path, args, expected_data: dict) -> TestResult:
    out_dir = Path(args.out_dir)
    rom_path = out_dir / f"{name}.rom"

    # Assemble
    ok, err = run_assembler(args.asm, asm_file, rom_path)
    if not ok:
        return TestResult(name, "ASM_FAIL", err)

    # Run emulator
    ok, stdout = run_emulator(args.emu, rom_path)
    if not ok:
        return TestResult(name, "RUN_FAIL", stdout)

    if args.verbose:
        print(f"  [{name}] emulator output:\n{stdout}\n")

    # Parse registers
    actual_regs, halted = parse_cpu_dump(stdout)
    if actual_regs is None:
        return TestResult(name, "PARSE_FAIL", stdout)

    if not halted:
        return TestResult(name, "WARN", "Exited by cycle limit (no HLT reached)",
                          actual_regs=actual_regs, halted=False)

    if args.capture:
        return TestResult(name, "CAPTURED", actual_regs=actual_regs)

    # Validate
    if name not in expected_data:
        return TestResult(name, "NO_EXPECTED", actual_regs=actual_regs)

    mismatches = compare_regs(expected_data[name], actual_regs)
    if mismatches:
        return TestResult(name, "FAIL", mismatches=mismatches, actual_regs=actual_regs)

    return TestResult(name, "PASS", actual_regs=actual_regs)


def format_regs_brief(regs: Dict[str, str], keys: Optional[list] = None) -> str:
    if not regs:
        return ""
    if keys is None:
        keys = [k for k in REG_NAMES if regs.get(k, "00") != "00"]
    return " ".join(f"{k}={regs[k]}" for k in keys if k in regs)


def print_report(results: List[TestResult]):
    print("\n=== Unit Test Results ===\n")
    for r in results:
        if r.status == "PASS":
            brief = format_regs_brief(r.actual_regs)
            print(f"[PASS] {r.name:20s} {brief}")
        elif r.status == "FAIL":
            details = ", ".join(
                f"{reg}: expected={exp} actual={act}"
                for reg, (exp, act) in r.mismatches.items()
            )
            print(f"[FAIL] {r.name:20s} {details}")
        elif r.status == "ASM_FAIL":
            print(f"[ASM_FAIL] {r.name:20s} {r.detail}")
        elif r.status == "RUN_FAIL":
            print(f"[RUN_FAIL] {r.name:20s} {r.detail}")
        elif r.status == "PARSE_FAIL":
            print(f"[PARSE_FAIL] {r.name:20s} Could not parse CPU dump")
        elif r.status == "NO_EXPECTED":
            brief = format_regs_brief(r.actual_regs)
            print(f"[NO_EXPECTED] {r.name:20s} {brief}")
        elif r.status == "WARN":
            print(f"[WARN] {r.name:20s} {r.detail}")
        elif r.status == "CAPTURED":
            brief = format_regs_brief(r.actual_regs)
            print(f"[CAPTURED] {r.name:20s} {brief}")


def print_summary(results: List[TestResult]):
    total = len(results)
    passed = sum(1 for r in results if r.status == "PASS")
    failed = sum(1 for r in results if r.status == "FAIL")
    errors = sum(1 for r in results if r.status in ("ASM_FAIL", "RUN_FAIL", "PARSE_FAIL"))
    warnings = sum(1 for r in results if r.status == "WARN")
    no_exp = sum(1 for r in results if r.status == "NO_EXPECTED")

    print(f"\n=== Summary ===")
    print(f"Total: {total}  Passed: {passed}  Failed: {failed}  Errors: {errors}", end="")
    if warnings:
        print(f"  Warnings: {warnings}", end="")
    if no_exp:
        print(f"  No expected: {no_exp}", end="")
    print()


def main():
    args = parse_args()

    # Resolve paths relative to script location (project root)
    script_dir = Path(__file__).resolve().parent.parent
    for attr in ("asm", "emu", "test_dir", "out_dir", "expected"):
        val = getattr(args, attr)
        p = Path(val)
        if not p.is_absolute():
            setattr(args, attr, str(script_dir / val))

    # Load expected values
    expected_data = {}
    if not args.capture:
        try:
            with open(args.expected, "r") as f:
                expected_data = json.load(f)
        except FileNotFoundError:
            print(f"Warning: expected.json not found at {args.expected}", file=sys.stderr)

    # Discover tests
    tests = discover_tests(args.test_dir, args.filter)
    if not tests:
        print("No test files found.", file=sys.stderr)
        sys.exit(1)

    # Create output directory
    os.makedirs(args.out_dir, exist_ok=True)

    # Run tests
    results = []
    for asm_file in tests:
        name = asm_file.stem
        result = run_test(name, asm_file, args, expected_data)
        results.append(result)

    # Output
    if args.capture:
        captured = {}
        for r in results:
            if r.actual_regs:
                # Only include non-zero registers
                captured[r.name] = {
                    k: v for k, v in r.actual_regs.items() if v != "00"
                }
        print(json.dumps(captured, indent=4, sort_keys=True))
    else:
        print_report(results)
        print_summary(results)

    all_ok = all(r.status in ("PASS", "CAPTURED") for r in results)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
