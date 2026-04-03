# Debug Symbols

The `--symbols` flag tells v6asm to write a `.symbols.json` file alongside the ROM.
This file maps assembled addresses back to source-level symbols and lines,
enabling features like hover information, definition lookup, and source-line
highlighting in editors and emulators.

```bash
v6asm main.asm --symbols      # produces main.symbols.json
```

---

## JSON Structure

The file contains three top-level sections.

### `symbols`

A map of symbol names to their metadata.

| Field  | Type   | Description |
|--------|--------|-------------|
| `value` | number | Resolved numeric value (address for labels/funcs, literal for consts, `-1` for macros and params without a default) |
| `path`  | string | Project-relative source file (forward slashes) |
| `line`  | number | 1-based source line number |
| `type`  | string | One of `label`, `const`, `func`, `macro`, `macroparam` |

**Symbol types:**

| Type | Emitted for |
|------|-------------|
| `label` | Global labels and local labels (`@name`) |
| `const` | Constants (`=` / `EQU`) and variables (`.var`) |
| `func`  | Labels inside `.optional` / `.endoptional` blocks (see [Functions](#functions) below) |
| `macro` | Macro definitions (`.macro`) |
| `macroparam` | Macro parameters, keyed as `MacroName.paramName` |

### `lineAddresses`

Maps each source file and line to the memory addresses produced by that line.

```
"<path>": {
  "<line>": [addr, ...]
}
```

A single line may produce multiple addresses (e.g. inside a `.loop`).

### `dataLines`

Describes lines that emit contiguous data bytes (`.byte`/`.db`, `.word`/`.dw`, etc.).

```
"<path>": {
  "<line>": {
    "addr": number,
    "byteLength": number,
    "unitBytes": number
  }
}
```

| Field | Description |
|-------|-------------|
| `addr` | Starting memory address |
| `byteLength` | Total bytes emitted by the line |
| `unitBytes` | Size of one element: 1 for `.byte`, 2 for `.word` |

---

## Naming Conventions

| Source construct | Symbol name |
|------------------|-------------|
| Global label `START:` | `START` |
| Constant `MAX_SIZE = 64` | `MAX_SIZE` |
| Local label `@loop` (1st occurrence) | `@loop_0` |
| Local label `@loop` (2nd occurrence) | `@loop_1` |
| Macro `PrintChar` | `PrintChar` |
| Macro param `ch` of `PrintChar` | `PrintChar.ch` |

Local labels are disambiguated by a zero-based occurrence index within the file.
The original spelling of the definition is preserved in the JSON keys.

---

## Functions

The assembler has no dedicated function syntax.
Instead, it uses `.optional` / `.endoptional` blocks to distinguish functions from plain labels.

A label inside an `.optional` block that is **referenced from outside** the block
is emitted as `"type": "func"` in the symbols file.
Only the **outermost** `.optional` block matters — nested blocks are ignored.

Labels that are never referenced from outside the block, or that live outside
any `.optional` block, are emitted as regular `"type": "label"`.

### Example

```asm
    .org 0x100
    call DrawSprite         ; reference from outside → makes it a func

    .optional
DrawSprite:                 ; ← emitted as func
    push b
@inner:                     ; ← emitted as label (local, not referenced externally)
    dcr c
    jnz @inner
    pop b
    ret
    .endoptional

PlainLabel:                 ; ← emitted as label (not inside .optional)
    nop
```

Resulting symbols (trimmed):

```json
{
  "DrawSprite":  { "value": 259, "path": "main.asm", "line": 5,  "type": "func"  },
  "@inner_0":    { "value": 260, "path": "main.asm", "line": 7,  "type": "label" },
  "PlainLabel":  { "value": 265, "path": "main.asm", "line": 13, "type": "label" }
}
```

---

## Paths

All paths in the JSON are **project-relative** with forward slashes,
so the file stays portable across machines.
The project root is the directory containing the source file passed to v6asm.

---

## Example

```json
{
  "symbols": {
    "START":         { "value": 256, "path": "main.asm",         "line": 3,  "type": "label"      },
    "MAX_SIZE":      { "value": 64,  "path": "main.asm",         "line": 5,  "type": "const"      },
    "@loop_0":       { "value": 270, "path": "main.asm",         "line": 12, "type": "label"      },
    "@loop_1":       { "value": 275, "path": "main.asm",         "line": 20, "type": "label"      },
    "Init":          { "value": 256, "path": "main.asm",         "line": 3,  "type": "func"       },
    "PrintChar":     { "value": -1,  "path": "main.asm",         "line": 45, "type": "macro"      },
    "PrintChar.ch":  { "value": -1,  "path": "main.asm",         "line": 45, "type": "macroparam" },
    "PrintChar.col": { "value": 7,   "path": "main.asm",         "line": 45, "type": "macroparam" },
    "SetBorder":     { "value": 512, "path": "test/palette.asm", "line": 1,  "type": "func"       }
  },
  "lineAddresses": {
    "main.asm": {
      "3":  [256],
      "6":  [257, 260],
      "12": [270],
      "20": [275]
    },
    "test/palette.asm": {
      "1": [512],
      "3": [514]
    }
  },
  "dataLines": {
    "main.asm": {
      "30": { "addr": 290, "byteLength": 8, "unitBytes": 1 },
      "31": { "addr": 298, "byteLength": 6, "unitBytes": 2 }
    },
    "test/palette.asm": {
      "10": { "addr": 520, "byteLength": 16, "unitBytes": 1 }
    }
  }
}
```
