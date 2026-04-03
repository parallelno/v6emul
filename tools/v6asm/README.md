# v6asm

> A two-pass Intel 8080 / subset Z80 assembler.

## Quick Start

```bash
v6asm -i main                  # scaffold a new project
v6asm main.asm                 # assemble → main.rom
v6asm main.asm -l              # + listing file
v6asm main.asm -s              # + debug symbols
v6asm main.asm -c z80          # Z80 mnemonic mode
v6asm -v                       # print build version
```

[![CI/CD](https://github.com/parallelno/v6asm/actions/workflows/ci.yml/badge.svg)](https://github.com/parallelno/v6asm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

---

## Overview

`v6asm` is a command-line toolchain for the **Vector-06c** (Вектор-06Ц).
It assembles `.asm` source files into `.rom` binaries and can build bootable **FDD disk images** for an emulator.

The assembler supports the Intel 8080 instruction set and an optional Z80 mnemonic
alternatives. A rich preprocessor handles file includes, macros, conditional
assembly, loops, optional code blocks,and more. The toolchain can also emit a
listing file for inspection and a `.symbols.json` for editor/debugger integration.

| Tool | Purpose |
|------|---------|
| `v6asm` | Assembler — `.asm` → `.rom` binary |
| `v6fdd` | FDD utility — packs files into a `.fdd` disk image |

## Installation

Download the latest archive from [Releases](https://github.com/parallelno/v6asm/releases), extract it, and add the directory to your `PATH`.

## Documentation

Full reference is in the [`docs/`](docs/README.md) folder:

- [CLI Usage](docs/cli.md) — arguments, options, output artifacts
- [Assembler Syntax](docs/syntax.md) — expressions, operators, literals, symbols
- [Directives](docs/directives.md) — `.org`, `.include`, `.if`, `.loop`, `.optional`, data emission, and more
- [Macros](docs/macros.md) — `.macro` / `.endmacro`, parameters, scoping
- [Listing Format](docs/listing.md) — `.lst` column layout and expansion behavior
- [Debug Symbols](docs/symbols.md) — `.symbols.json` schema, symbol types, and naming conventions

### Build from source

Requires the [Rust toolchain](https://rustup.rs/) (stable).

```bash
git clone https://github.com/parallelno/v6asm.git
cd v6asm
cargo build --release
```

## Tests

```bash
cargo test --workspace
```

## License

[MIT](LICENSE)
