# v6emul Documentation

**v6emul** is a C++ CLI emulator for the **Vector-06C** — a Soviet home computer built around the Intel 8080 (KR580VM80A) CPU. It runs as a headless backend, serving external frontends (VS Code extension, standalone GUI) over TCP IPC, and doubles as a test harness for [v6_assembler](https://github.com/parallelno/v6_assembler).

## Documentation Index

| Document | Description |
|----------|-------------|
| [Building](building.md) | Build prerequisites, CMake configuration, and compilation |
| [CLI Reference](cli.md) | Command-line options, modes, and examples |
| [IPC Protocol](ipc-protocol.md) | Wire format, message framing, command reference |
| [Architecture](architecture.md) | Thread model, library hierarchy, code structure |
| [Test Client](test-client.md) | Win32 test client for frame display and debugging |

## Quick Start

```bash
# Build (Release)
cmake --preset release
cmake --build --preset release

# Run with IPC server
v6emul --serve

# Run with a ROM
v6emul --rom game.rom --load-addr 0x100 --serve

# Headless test mode
v6emul --rom test.rom --halt-exit --dump-cpu
```

## Origin

The emulator core is forked from [Devector](https://github.com/parallelno/Devector). The core and utils libraries compile with zero GUI dependencies — the WPF build in Devector proves this separation is structural, not conditional.
