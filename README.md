# v6emul

> A command-line emulator for the Vector-06C Soviet PC.

## Quick Start

```bash
./build/release/app/v6emul --serve            # start IPC server mode
./build/release/app/v6emul --version          # print build version
./build/release/app/v6emul --help             # show CLI help
./build/release/app/v6emul --boot-rom res/boot/boot.bin --serve
./build/release/app/v6emul --fdd game.fdd --boot-rom res/boot/boot.bin --fdd-autoboot --serve
./build/release/app/v6emul --rom test.rom --halt-exit --dump-cpu
```

[![CI](https://github.com/parallelno/v6emul/actions/workflows/ci.yml/badge.svg)](https://github.com/parallelno/v6emul/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

---

## Overview

`v6emul` is a headless emulator for the **Vector-06C** (Вектор-06Ц), a Soviet home computer built around the Intel 8080-compatible CPU.
It runs as a command-line process, can serve external frontends over TCP IPC, and also works as a deterministic test runner for ROM-based and assembly-level validation.

The project is split into reusable libraries for the emulator core, IPC transport, and shared utilities. In practice that means the same backend can power automated tests, a standalone client, or editor tooling without pulling GUI code into the core runtime.

| Component | Purpose |
|------|---------|
| `v6emul` | Main emulator executable with banner, test, and IPC server modes |
| `test_client` | Minimal client for connecting to the emulator and displaying frames |

## Installation

Download the latest archive from [Releases](https://github.com/parallelno/v6emul/releases), extract it, and run `v6emul` directly or add the extracted directory to your `PATH`.

## Documentation

Full reference is in the [`docs/`](docs/README.md) folder:

- [CLI Usage](docs/cli.md) — arguments, modes, and command examples
- [Test Client](docs/test-client.md) — running and using the sample client
- [Building](docs/building.md) — prerequisites, presets, and test commands
- [IPC Protocol](docs/ipc-protocol.md) — wire format, commands, and framing

### Build from source

Requires CMake 3.21+, a C++20 compiler, Git, and Python 3.8+ for the ASM unit test runner.

```bash
git clone https://github.com/parallelno/v6emul.git
cd v6emul
cmake --preset release
cmake --build --preset release
```

## Tests

```bash
ctest --test-dir build/release --build-config Release --output-on-failure
```

## License

[MIT](LICENSE)
