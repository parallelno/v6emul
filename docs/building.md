# Building

## Prerequisites

- **CMake** 3.21 or later
- **C++20** compiler:
  - MSVC 2022 (Windows)
  - GCC 11+ (Linux)
  - MinGW-w64 (Windows, alternative)
  - AppleClang/Xcode Command Line Tools (macOS)
- **Git** and an internet connection (for fetching dependencies)
- **Python** 3.8+ (required during CMake configuration and by the ASM unit test runner)
- Platform build tool used by LuaJIT: Visual Studio/MSBuild, `make`, or `mingw32-make`

## Dependencies

All dependencies are fetched automatically during CMake configuration:

| Dependency | Version | Method |
|------------|---------|--------|
| nlohmann/json | 3.11.3 | FetchContent (header-only) |
| LuaJIT | 2.1.0-beta3 | FetchContent + ExternalProject (static lib) |

The C++ dependencies do not need to be installed separately. CMake downloads their
sources into the selected build directory during the first configuration. The
platform compiler and build tools listed above must already be installed.

### Python (test runner)

The CMake project discovers Python while configuring the tests, so **Python 3.8+**
must be available even when only building the emulator. The ASM test runner uses
only the standard library; no third-party Python packages are needed.

## Configure & Build

The project uses **CMake Presets**. Three configurations are available:

| Preset | Build Type | Binary Dir |
|--------|-----------|------------|
| `debug` | Debug | `build/debug/` |
| `release` | Release | `build/release/` |
| `ci` | Release | `build/ci/` |

### Build From a Clean Checkout

Run every command in this section from the repository root, the directory that
contains `CMakePresets.json`. A pre-existing `build/` directory is not required:
the configure command creates `build/release/`, downloads the dependencies, and
generates the native build files.

```bash
# 1. Clone and enter the repository (skip if the source is already checked out)
git clone https://github.com/parallelno/v6emul.git
cd v6emul

# 2. Configure. This creates build/release; it may take time on the first run.
cmake --preset release

# 3. Compile the emulator, test programs, and Windows test client.
cmake --build --preset release

# 4. Run the complete CTest suite.
ctest --test-dir build/release --build-config Release --output-on-failure
```

Configuration must succeed before the build command is run. If a build directory
contains stale or incomplete generated files, remove that preset's directory
(for example, `build/release/`) and repeat steps 2-4.

### Debug Build

```bash
cmake --preset debug
cmake --build --preset debug
ctest --test-dir build/debug --build-config Debug --output-on-failure
```

### Parallel Compilation

Parallel builds are enabled by default:

- **MSVC**: `/MP` compiler flag is set in the root `CMakeLists.txt`
- **All presets**: `"jobs": 0` in `CMakePresets.json` (uses all available cores)

No additional flags are needed — just `cmake --build --preset <name>`.

## Output

Visual Studio is a multi-config generator and adds the configuration directory to
the output path. Make and Ninja are single-config generators and do not.

| Generator/platform | Release emulator | Debug emulator |
|--------------------|------------------|----------------|
| Visual Studio | `build/release/app/Release/v6emul.exe` | `build/debug/app/Debug/v6emul.exe` |
| Make/Ninja on Windows | `build/release/app/v6emul.exe` | `build/debug/app/v6emul.exe` |
| Make/Ninja on Linux/macOS | `build/release/app/v6emul` | `build/debug/app/v6emul` |

The Windows-only test client follows the same layout under
`build/<preset>/tools/test_client/`. For example, an MSVC release build produces:

- `build/release/tools/test_client/Release/test_client.exe`

## Running Tests

```bash
# Configure and build first, then run all tests and print details for failures.
ctest --test-dir build/release --build-config Release --output-on-failure
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
| `golden_test_boot_rom` | Golden-file test for boot ROM startup |
| `asm_unit_tests` | Assembly-level Intel 8080 instruction tests (when `v6asm` is available) |

Golden tests run the emulator with `--halt-exit` on a test ROM and compare stdout output against expected files in `tests/golden/`.

### ASM Unit Tests

The portable way to run the assembly-level CPU instruction tests after a release
build is through CTest. CMake supplies the correct emulator, assembler, and Python
paths for the selected generator:

```bash
ctest --test-dir build/release --build-config Release -R asm_unit_tests --output-on-failure
```

To invoke the Python runner directly with a Visual Studio release build (its
default layout):

```bash
python tests/run_unit_tests.py
```

For a single-config generator, pass its emulator path explicitly:

```bash
# Windows with MinGW or Ninja
python tests/run_unit_tests.py --emu build/release/app/v6emul.exe

# Linux or macOS
python3 tests/run_unit_tests.py --emu build/release/app/v6emul
```

Add `--verbose` to show full emulator output per test:

```bash
python tests/run_unit_tests.py --verbose
```

Recapture actual register values (for updating `expected.json`):

```bash
python tests/run_unit_tests.py --capture
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
tests/
  tools/
    v6asm/              Assembler binary used by ASM unit tests.
  unit_tests/           Assembly source files and expected.json for ASM tests.
  golden/               Expected stdout files for golden tests.
```
