# v6emul

`v6emul` is a Vector-06C emulator backend written in C++20. It exposes a TCP IPC server for frontends (test client, IDE integration, automation).

## Quick start

```bash
cmake --preset release
cmake --build --preset release

# Run emulator server
build/release/app/Release/v6emul.exe --serve

# Run test client (default port 9876)
build/release/tools/test_client/Release/test_client.exe
```

## Documentation

- [Documentation hub](docs/README.md)
- [CLI reference](docs/cli.md)
- [IPC protocol](docs/ipc-protocol.md)
- [Architecture](docs/architecture.md)
- [Building](docs/building.md)
- [Test client](docs/test-client.md)

## Project structure

- `app/` - main executable and CLI startup
- `libs/v6core/` - emulator core (CPU, memory, display, IO, audio, debug)
- `libs/v6ipc/` - TCP server and public command protocol
- `libs/v6utils/` - shared utilities (queues, args parser, helpers)
- `tools/test_client/` - lightweight Win32 frame viewer
- `tests/` - unit/integration/e2e/golden tests
