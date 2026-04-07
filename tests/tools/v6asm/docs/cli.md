# CLI Usage

```
v6asm <source.asm> [options]
v6asm --init <name>
```

## Arguments

| Argument | Description |
|----------|-------------|
| `<source>` | Assembly source file (`.asm`) to compile |
| `-i`, `--init <name>` | Scaffold a new `.asm` file with a starter template |
| `-o`, `--output <path>` | Output ROM path (default: `<source>.rom`) |
| `-c`, `--cpu <cpu>` | Target CPU: `i8080` (default) or `z80` |
| `-a`, `--rom-align <n>` | ROM size alignment in bytes (default: `1`) |
| `-q`, `--quiet` | Suppress `.print` output |
| `-V`, `--verbose` | Extra diagnostics |
| `-l`, `--lst` | Generate a listing file (`.lst`) alongside the ROM |
| `-s`, `--symbols` | Generate a debug symbols file (`.symbols.json`) alongside the ROM |
| `-v`, `--version` | Print the build version string |
| `-h`, `--help` | Print help with the program header |

## Help And Version

`-v` / `--version` prints the build version in the form `YYYY.MM.DD-<git-hash>`.

`-h` / `--help` prints the normal clap-generated help preceded by:

```text
Intel 8080/Z80 assembler, version <version>
(c) Aleksandr Fedotovskikh <mailforfriend@gmail.com>
```

## Examples

```bash
v6asm main.asm                        # compile, output main.rom
v6asm -i main                         # create main.asm from template
v6asm main.asm -o out/program.rom     # custom output path
v6asm main.asm -c z80 -l              # Z80 mode + listing
v6asm main.asm -s                     # generate debug symbols
v6asm -v                              # print version
```

## Output Artifacts

- `<name>.rom` — Vector 06c executable loaded by the emulator.
- `<name>.lst` — optional listing file (enabled with `--lst`) showing addresses, emitted bytes, and source lines. See [Listing File Format](listing.md) for details.
- `<name>.symbols.json` — optional debug symbols file (enabled with `--symbols`) containing source-level symbol information for debuggers and editors. Maps labels, constants, variables, macros, and functions back to source locations, and records per-line address and data mappings.
