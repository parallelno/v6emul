# CLI Reference

## Usage

```
v6emul [OPTIONS]
```

## Modes

v6emul operates in one of three modes depending on the flags provided:

| Mode | Trigger | Behavior |
|------|---------|----------|
| **Banner** | No mode flags | Prints usage hint and exits |
| **Test** | `--halt-exit`, `--run-frames`, or `--run-cycles` | Loads ROM, runs headlessly, prints results to stdout, exits |
| **Server** | `--serve` | Starts TCP IPC server, runs emulation, accepts client connections |

## Options

### ROM Loading

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--boot-rom <path>` | string | *(none)* | Path to a boot ROM file mapped at address `0x0000` on startup and after `RESET` |
| `--rom <path>` | string | *(none)* | Path to a ROM file to load into emulator memory |
| `--load-addr <addr>` | int | `0` | Memory address to load the ROM at. Supports hex (`0x100`) and decimal (`256`) |

### FDD (Floppy Disk) Loading

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--fdd <path>` | string | *(none)* | Path to a floppy disk image to mount |
| `--fdd-drive <N>` | int | `0` | FDD drive index (0–3) |
| `--fdd-autoboot` | flag | — | Reset and boot from the mounted floppy disk |

`--fdd` loads a floppy disk image file, pads or truncates it to the standard FDD size (819,200 bytes), and mounts it on the specified drive. With `--fdd-autoboot`, the emulator performs a `RESET` after mounting so the boot ROM (if present) can boot from the floppy.

`--boot-rom` loads a ROM overlay through the core memory subsystem. It is active at startup and after `RESET`, and it is disabled by `RESTART`. In test mode, `--rom` still loads into RAM and switches execution into RAM mode before running.

### Execution Control

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--serve` | flag | — | Start the TCP IPC server |
| `--speed <speed>` | string | *(normal)* | Execution speed: `1%`, `20%`, `50%`, `100%`, `200%`, `max` |
| `--frame-format <fmt>` | string | `rgba` | Pixel format for `GET_FRAME_RAW`: `rgba` or `bgra` |
| `--frame-mode <mode>` | string | `bordered` | Frame region returned by IPC: `full`, `bordered`, `borderless` |
| `--tcp-port <port>` | int | `9876` | TCP port for the IPC server |

### Stop Conditions (Test Mode)

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--halt-exit` | flag | — | Exit on first HLT instruction |
| `--run-frames <N>` | int | `0` | Run for N frames then exit |
| `--run-cycles <N>` | int | `0` | Run for N CPU cycles then exit |

### Dump Flags (Test Mode)

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--dump-cpu` | flag | — | Print full CPU state on exit (registers, flags, PC, SP, cycles) |
| `--dump-memory` | flag | — | Print full 64K memory dump (hex) on exit |
| `--dump-ramdisk <N>` | int | `-1` | Print RAM-disk N (0–7) contents on exit |

### Other

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--log-level <level>` | string | `info` | Log verbosity: `error`, `warn`, `info`, `debug`, `trace` |
| `--version`, `-V` | flag | — | Print version and exit |
| `-h`, `--help` | flag | — | Print help and exit |

## Examples

### Start IPC server with a ROM at address 0x100

```bash
v6emul --rom game.rom --load-addr 0x100 --serve
```

### Start IPC server with a boot ROM

```bash
v6emul --boot-rom res/boot/boot.bin --serve
```

### Run at maximum speed

```bash
v6emul --rom game.rom --load-addr 0x100 --serve --speed max
```

### Headless test: run until HLT, dump CPU state

```bash
v6emul --rom test.rom --halt-exit --dump-cpu
```

### Run for exactly 1000 frames

```bash
v6emul --rom test.rom --run-frames 1000 --dump-cpu
```

### Mount a floppy disk image

```bash
v6emul --boot-rom res/boot/boot.bin --fdd game.fdd --serve
```

### Mount a floppy disk on drive 1 and autoboot

```bash
v6emul --boot-rom res/boot/boot.bin --fdd game.fdd --fdd-drive 1 --fdd-autoboot --serve
```

### Use a custom TCP port

```bash
v6emul --rom game.rom --serve --tcp-port 12345
```

### Stream borderless frames (active area only, 512×256)

```bash
v6emul --rom game.rom --serve --frame-mode borderless
```

### Stream full frames including vsync region (768×312)

```bash
v6emul --rom game.rom --serve --frame-mode full
```

### Stream bordered frames (active area + 16px border on each side → 544×288)

```bash
v6emul --rom game.rom --serve --frame-mode bordered
```

## Test Output Format

In test mode, the emulator captures `OUT` instructions to the test port (`0xED`) and prints them to stdout:

```
TEST_OUT port=0xED value=0x42
TEST_OUT port=0xED value=0x00
HALT at PC=0x0105 after 847231 cpu_cycles 1200 frames
```

The exit line reports the stop reason (`HALT` or `EXIT`), the program counter, cycle count, and frame count. This format is consumed by test runners for deterministic assertions.

## Error Handling

- Unknown arguments produce an error message and print the help text.
- Missing required argument values print a requirement message.
- ROM load failures print an error and exit with code 1.
- Boot ROM load failures print an error and exit with code 1.
- FDD image load failures print an error and exit with code 1.
- Invalid `--fdd-drive` values (outside 0–3) print an error and exit with code 1.
- Invalid `--frame-mode` values print an error and exit with code 1.
