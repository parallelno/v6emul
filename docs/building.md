# Building

## Prerequisites

- **CMake** 3.21 or later
- **C++20** compiler:
  - MSVC 2022 (Windows)
  - GCC 11+ (Linux)
  - MinGW-w64 (Windows, alternative)
- **Git** (for fetching dependencies)
- **Python** 3.8+ (for ASM unit test runner)

## Dependencies

All dependencies are fetched automatically during CMake configuration:

| Dependency | Version | Method |
|------------|---------|--------|
| nlohmann/json | 3.11.3 | FetchContent (header-only) |
| LuaJIT | 2.1.0-beta3 | FetchContent + ExternalProject (static lib) |

No manual dependency installation is required.

### Python (test runner)

The ASM unit test suite (`tests/run_unit_tests.py`) requires **Python 3.8+**. It uses only the standard library — no third-party packages are needed.

## Configure & Build

The project uses **CMake Presets**. Three configurations are available:

| Preset | Build Type | Binary Dir |
|--------|-----------|------------|
| `debug` | Debug | `build/debug/` |
| `release` | Release | `build/release/` |
| `ci` | Release | `build/ci/` |

### Quick Start

```bash
# Configure
cmake --preset release

# Build
cmake --build --preset release
```

### Debug Build

```bash
cmake --preset debug
cmake --build --preset debug
```

### Parallel Compilation

Parallel builds are enabled by default:

- **MSVC**: `/MP` compiler flag is set in the root `CMakeLists.txt`
- **All presets**: `"jobs": 0` in `CMakePresets.json` (uses all available cores)

No additional flags are needed — just `cmake --build --preset <name>`.

## Output

The built executable is located at:

- **Release**: `build/release/app/Release/v6emul.exe` (MSVC) or `build/release/app/v6emul` (GCC/MinGW)
- **Debug**: `build/debug/app/Debug/v6emul.exe` (MSVC) or `build/debug/app/v6emul` (GCC/MinGW)

The test client executable is at:

- `build/release/tools/test_client/Release/test_client.exe`

## Running Tests

```bash
# Build first, then run all tests
cmake --build --preset release
ctest --test-dir build/release --build-config Release
```

### Test Suites

| Test | Description |
|------|-------------|
| `cpu_tests` | Intel 8080 CPU instruction set |
| `memory_tests` | Memory subsystem, RAM disks, mapping |
| `integration_tests` | Multi-subsystem interactions |
| `determinism_tests` | Reproducibility of emulation results |
| `ipc_tests` | IPC serialization and transport |
| `e2e_tests` | End-to-end server + client |
| `golden_test_port` | Golden-file test for port I/O ROM |
| `golden_test_arith` | Golden-file test for arithmetic ROM |

Golden tests run the emulator with `--halt-exit` on a test ROM and compare stdout output against expected files in `tests/golden/`.

### ASM Unit Tests

Run the assembly-level CPU instruction tests separately:
```bash
python tests/run_unit_tests.py
```
Show full emulator output per test
```bash
python tests/run_unit_tests.py --verbose
```
Recapture actual register values (for updating expected.json)
```bash
python tests/run_unit_tests.py --capture
```

Or via CTest:

```bash
ctest --test-dir build/release --build-config Release -R asm_unit_tests
```

See [PLAN_unit_test_suite_2026-03-31.md](../PLAN_unit_test_suite_2026-03-31.md) for the full test suite design.

## Project Structure

```
CMakeLists.txt          Root build. Dependencies, sub-projects, testing.
CMakePresets.json       Configure/build presets (debug, release, ci).
app/
  CMakeLists.txt        v6emul executable, links v6core + v6ipc + v6utils.
  main.cpp              CLI entry point.
libs/
  v6core/               Emulation engine (CPU, Memory, Display, IO, Audio, FDC, Scripts).
  v6ipc/                TCP transport + MessagePack protocol.
  v6utils/              Shared utilities (types, queue, args parser, file I/O).
tools/
  test_client/          Win32 GDI display client.
tests/                  Unit, integration, e2e, and golden tests.
```
