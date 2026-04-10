# Assembler Syntax

By default the assembler targets the Intel 8080 instruction set. Use `--cpu z80` to enable a compatibility subset of Zilog Z80 mnemonics that map 1:1 to the 8080 instruction encodings (including Z80 aliases like `LD (N),A` and `ADD HL,BC`). Pure Z80-only features such as IX/IY indexed addressing are not supported.

## Case Sensitivity

The assembler is **case-insensitive** for all identifiers:

- **Mnemonics**: `MOV`, `mov`, and `Mov` are equivalent.
- **Registers**: `HL`, `hl`, and `Hl` all refer to the same register pair.
- **Directives**: `.ORG`, `.org`, and `.Org` are the same directive.
- **Symbols**: `Start` and `start` refer to the same label; `MAX_SIZE` and `max_size` are the same constant.
- **Macros**: `SetPalette` and `SETPALETTE` invoke the same macro.

Original case is preserved in listing output and debug symbols (`.symbols.json`).

## Comments

The assembler supports two comment styles:

- **Single-line comments**: Start with `;` or `//` and continue to the end of the line.
  ```asm
  mvi a, 0x10  ; Load accumulator with 0x10
  mvi b, 0x20  // Load register B with 0x20
  ```

- **Multi-line comments**: Enclosed between `/*` and `*/`, can span multiple lines or be used inline.
  ```asm
  /* This is a multi-line comment
     that spans multiple lines
     and is ignored by the assembler */
  mvi a, 0x10

  mvi b, 0x20  /* inline multi-line comment */
  ```

Multi-line comments are stripped during preprocessing and work correctly with string literals, escaped characters, and can be placed anywhere in the code.

## Expressions and Operators

The assembler supports a rich expression system used throughout directives (`.if`, `.loop`, `.align`, `.print`, etc.), immediate values, and address calculations. Expressions can combine numeric literals, symbols, and operators.

### Numeric Literals

| Format | Example | Description |
|--------|---------|-------------|
| Decimal | `42`, `-5` | Standard decimal numbers |
| Hex `$` | `$FF`, `$1234` | Hexadecimal with `$` prefix |
| Hex `0x` | `0xFF`, `0x1234` | Hexadecimal with `0x` prefix |
| Hex `h` | `0FFh`, `07Fh` | Hexadecimal with `h` suffix (must start with digit) |
| Binary `%` | `%1010`, `%11_00` | Binary with `%` prefix (underscores allowed) |
| Binary `0b` | `0b1010`, `0b11_00` | Binary with `0b` prefix (underscores allowed) |
| Binary `b` | `b1010`, `b11_00` | Binary with `b` prefix (underscores allowed) |
| Character | `'A'`, `'\n'` | ASCII character (supports escapes) |

### Arithmetic Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `+` | Addition | `Value + 10` |
| `-` | Subtraction | `EndAddr - StartAddr` |
| `*` | Multiplication | `Count * 2` |
| `/` | Integer division (truncates toward zero) | `Total / 4`, `-5 / 2` → `-2` |
| `%` | Modulo (integer remainder) | `Offset % 256`, `14 % 4` → `2` |

### Comparison Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `==` | Equal | `Value == 0` |
| `!=` | Not equal | `Flag != FALSE` |
| `<` | Less than | `Count < 10` |
| `<=` | Less than or equal | `Index <= Max` |
| `>` | Greater than | `Size > 0` |
| `>=` | Greater than or equal | `Addr >= $100` |

### Bitwise Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `&` | Bitwise AND | `Value & $0F` |
| `\|` | Bitwise OR | `Flags \| $80` |
| `^` | Bitwise XOR | `Data ^ $FF` |
| `~` | Bitwise NOT | `~Mask` |
| `<<` | Left shift | `1 << 4` |
| `>>` | Right shift | `Value >> 8` |

### Logical Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `!` | Logical NOT | `!Enabled` |
| `&&` | Logical AND | `(A > 0) && (B < 10)` |
| `\|\|` | Logical OR | `(X == 0) \|\| (Y == 0)` |

### Unary Prefix Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `+` | Unary plus (identity) | `+Value` |
| `-` | Unary minus (negation) | `-Offset` |
| `<` | Low byte (bits 0-7) | `<$1234` → `$34` |
| `>` | High byte (bits 8-15) | `>$1234` → `$12` |

The `<` (low byte) and `>` (high byte) unary operators extract 8-bit portions from 16-bit values. This is useful for splitting addresses or constants when working with 8-bit instructions:

```
ADDR = $1234

mvi l, <ADDR    ; Load low byte ($34) into L
mvi h, >ADDR    ; Load high byte ($12) into H

db <$ABCD       ; Emits $CD
db >$ABCD       ; Emits $AB
```

### Symbols

Expressions can reference:
- Labels (e.g., `StartAddr`, `Loop`)
- Constants defined with `=` or `EQU` (e.g., `MAX_VALUE`)
- Local labels prefixed with `@` (e.g., `@loop`)
- Boolean literals `TRUE` (1) and `FALSE` (0)
- Location counter `*`, which resolves to the current address for any expression (constants, data, immediates, directives). Example:

```asm
.org $0100
lxi h, * + 1 ; hl => $101
```

### Reserved Identifiers

Register names and their indirect forms are treated as operands, so they are not valid label names. Avoid using any of these as labels and labels shorter than four symbols in general:

  `A`,`B`,`C`,`D`,`E`,`H`,`L`,
  `BC`,`DE`,`HL`,`SP`,`AF`, `AF'`, `PSW`,
  `(A)`,`(B)`,`(C)`,`(D)`,`(E)`,`(H)`,`(L)`, `M`,
  `(BC)`, `(DE)`, `(HL)`, `(SP)`,
  `IX`, `IY`, `(IX)`, `(IY)`, `IXH`, `IXL`, `IYH`, `IYL`, `I`, `R`,
  `NZ`, `Z`, `NC`, `C`, `PO`, `PE`, `P`

### Operator Precedence

From highest to lowest:

1. Parentheses `()`
2. Unary operators: `+`, `-`, `!`, `~`, `<`, `>`
3. Multiplicative: `*`, `/`, `%`
4. Additive: `+`, `-`
5. Shift: `<<`, `>>`
6. Relational: `<`, `<=`, `>`, `>=`
7. Equality: `==`, `!=`
8. Bitwise AND: `&`
9. Bitwise XOR: `^`
10. Bitwise OR: `|`
11. Logical AND: `&&`
12. Logical OR: `||`
