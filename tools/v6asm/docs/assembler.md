# Intel 8080/Z80 Assembler:

## CLI Usage

```
v6asm <source.asm> [options]
v6asm --init <name>
```

### Arguments

| Argument | Description |
|----------|-------------|
| `<source>` | Assembly source file (`.asm`) to compile |
| `--init <name>` | Scaffold a new `.asm` file with a starter template |
| `-o`, `--output <path>` | Output ROM path (default: `<source>.rom`) |
| `--cpu <cpu>` | Target CPU: `i8080` (default) or `z80` |
| `--rom-align <n>` | ROM size alignment in bytes (default: `1`) |
| `-q`, `--quiet` | Suppress `.print` output |
| `-v`, `--verbose` | Extra diagnostics |
| `--lst` | Generate a listing file (`.lst`) alongside the ROM |

### Examples

```bash
v6asm main.asm                        # compile, output main.rom
v6asm main.asm -o out/program.rom     # custom output path
v6asm main.asm --cpu z80 --lst        # Z80 mode + listing
v6asm --init main                     # create main.asm from template
```

## Output Artifacts

- `<name>.rom` — Vector 06c executable loaded by the emulator.
- `<name>.lst` — optional listing file (enabled with `--lst`) showing addresses, emitted bytes, and source lines.

## Assembler
By default the assembler targets the Intel 8080 instruction set. Use `--cpu z80` to enable a compatibility subset of Zilog Z80 mnemonics that map 1:1 to the 8080 instruction encodings (including Z80 aliases like `LD (N),A` and `ADD HL,BC`). Pure Z80-only features such as IX/IY indexed addressing are not supported.

### Comments

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

### Expressions and Operators

The assembler supports a rich expression system used throughout directives (`.if`, `.loop`, `.align`, `.print`, etc.), immediate values, and address calculations. Expressions can combine numeric literals, symbols, and operators.

**Numeric Literals**

| Format | Example | Description |
|--------|---------|-------------|
| Decimal | `42`, `-5` | Standard decimal numbers |
| Hex `$` | `$FF`, `$1234` | Hexadecimal with `$` prefix |
| Hex `0x` | `0xFF`, `0x1234` | Hexadecimal with `0x` prefix |
| Binary `%` | `%1010`, `%11_00` | Binary with `%` prefix (underscores allowed) |
| Binary `0b` | `0b1010`, `0b11_00` | Binary with `0b` prefix (underscores allowed) |
| Binary `b` | `b1010`, `b11_00` | Binary with `b` prefix (underscores allowed) |
| Character | `'A'`, `'\n'` | ASCII character (supports escapes) |

**Arithmetic Operators**

| Operator | Description | Example |
|----------|-------------|---------|
| `+` | Addition | `Value + 10` |
| `-` | Subtraction | `EndAddr - StartAddr` |
| `*` | Multiplication | `Count * 2` |
| `/` | Integer division (truncates toward zero) | `Total / 4`, `-5 / 2` → `-2` |
| `%` | Modulo (integer remainder) | `Offset % 256`, `14 % 4` → `2` |

**Comparison Operators**

| Operator | Description | Example |
|----------|-------------|---------|
| `==` | Equal | `Value == 0` |
| `!=` | Not equal | `Flag != FALSE` |
| `<` | Less than | `Count < 10` |
| `<=` | Less than or equal | `Index <= Max` |
| `>` | Greater than | `Size > 0` |
| `>=` | Greater than or equal | `Addr >= $100` |

**Bitwise Operators**

| Operator | Description | Example |
|----------|-------------|---------|
| `&` | Bitwise AND | `Value & $0F` |
| `\|` | Bitwise OR | `Flags \| $80` |
| `^` | Bitwise XOR | `Data ^ $FF` |
| `~` | Bitwise NOT | `~Mask` |
| `<<` | Left shift | `1 << 4` |
| `>>` | Right shift | `Value >> 8` |

**Logical Operators**

| Operator | Description | Example |
|----------|-------------|---------|
| `!` | Logical NOT | `!Enabled` |
| `&&` | Logical AND | `(A > 0) && (B < 10)` |
| `\|\|` | Logical OR | `(X == 0) \|\| (Y == 0)` |

**Unary Prefix Operators**

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

**Symbols**

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

**Reserved identifiers (cannot be labels)**

Register names and their indirect forms are treated as operands, so they are not valid label names. Avoid using any of these as labels and labels shorter than four symbols in general:

  `A`,`B`,`C`,`D`,`E`,`H`,`L`,
  `BC`,`DE`,`HL`,`SP`,`AF`, `AF'`, `PSW`,
  `(A)`,`(B)`,`(C)`,`(D)`,`(E)`,`(H)`,`(L)`, `M`,
  `(BC)`, `(DE)`, `(HL)`, `(SP)`,
  `IX`, `IY`, `(IX)`, `(IY)`, `IXH`, `IXL`, `IYH`, `IYL`, `I`, `R`,
  `NZ`, `Z`, `NC`, `C`, `PO`, `PE`, `P`

**Operator Precedence** (highest to lowest)

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

### Directives
- `.org` directive: supported (decimal, `0x..`, or `$..`). Example: `.org 0x100`.

- `.include` directive: include another file inline using `.include "file.asm"` or `.include 'file.asm'`. Includes can be relative paths.In that case they are resolved relative to the including file, the main asm file, or the workspace directory. Includes support recursive expansion up to 16 levels.

- `.filesize Name, "path"`: defines a constant `Name` equal to the byte size of the given file. The path resolves like `.include` (current file, main asm, project folder, workspace, then CWD).

  Example:

  ```asm
  ROM_SIZE .filesize "out/prg.rom" ; ROM_SIZE becomes the size of prg.rom
  BufEnd   = BufStart + ROM_SIZE    ; use it in expressions
  ```

- `.incbin` directive: include raw bytes from an external file at the current address. Syntax: `.incbin "path"[ , offset[, length]]`. Paths resolve like `.include`. `offset` and `length` are optional expressions (decimal, hex, or binary); omit them to start at 0 and read the entire file.

- `.macro` / `.endmacro`: build parameterized macros (with defaults, nested calls, and per-invocation label namespaces) that expand inline before assembly.

- `.if` / `.endif`: wrap any sequence of source lines in a conditional block that assembles only when its expression evaluates to non-zero. You can nest `.if` directives freely, and the parser short-circuits inactive branches so forward references inside skipped blocks do not trigger errors. The argument may be a single numeric/boolean literal or any full expression evaluated with the rules below. Expressions support decimal/hex/binary literals, character constants, symbol names (labels, constants, `@local` labels), arithmetic (`+ - * / % << >>`), comparisons (`== != < <= > >=`), bitwise logic (`& | ^ ~`), and boolean operators (`! && ||`). Example:

```
Value = 3

.if (Value >= 2) && (SomeFlag == TRUE)
  mvi a, #$00
  sta $d020
.endif
```

- `.loop` / `.endloop` (short: `.endl`): repeat a block of source lines `LoopCount` times (maximum per loop: 100,000). Loop counts are evaluated with the same expression engine as `.if`, so you can reference previously defined constants or simple expressions. Loop bodies can nest other `.loop` or `.if` blocks, and any constant assignments inside the block execute on each iteration because the assembler expands the body inline:

```
Value = 0
Step  = 4

.loop (Step / 2)
  db Value
  Value = Value + 1
.endl
```

The example above emits `Value` three times (0, 1, 2) and leaves `Value` set to 3 for subsequent code.

- `.optional` / `.endoptional` (short: `.opt` / `.endopt`): defines an optional code block that is automatically removed from output if none of its internal labels and constants are used externally. Example:

```
.optional
useless_byte:
  db 0       ; removed if useless_byte is never referenced
.endoptional
```
```
call useful_routine
.opt
useful_routine:
  mvi a, 1 ; kept because useful_routine label was used
  ret      ; kept because useful_routine label was used
.endopt
```

- `.setting key, value [, key2, value2 ...]`: updates assembler defaults using non-case-sensitive key/value pairs. Values may be string, integer, or boolean. Multiple pairs can be specified in one directive.
```
.setting optional, false ; disables pruning of `.optional` blocks (the markers are stripped, but bodies stay).
```

- **Local labels and constants (`@name`)**: locals are scoped between the nearest surrounding global labels (or the start/end of the file/macro/loop expansion). A reference resolves to the closest definition in the same scope, preferring the latest definition at or before the reference; if none, it falls back to the next definition in that scope. Locals are per-file/per-macro and do not collide with globals. Example:

```
mem_erase_sp_filler:
  lxi b, $0000
  sphl
  mvi a, 0xFF
@loop:
  PUSH_B(16)
  dcx d
  cmp d
  jnz @loop    ; resolves to @loop above (same scope)

mem_fill_sp:              ; new global label -> new local scope
  shld mem_erase_sp_filler + 1
  ; @loop here would be unrelated to the one above
```

Locals can be redefined in a scope; references before a redefinition bind to the earlier definition, references after bind to the later one. Use globals for cross-scope jumps or data addresses.

- `.print`: emit compile-time diagnostics to the console during the second pass. Arguments are comma-separated and can mix string literals (`"PC="`), numeric literals, labels, or arbitrary expressions. Each argument is evaluated with the same expression engine as `.if`, so you can dump intermediate values or addresses while assembling:

```
.print "Copying from", SourceAddr, "to", DestAddr
.print "Loop count:", (EndAddr - StartAddr) / 16
```

Strings honor standard escapes (`\n`, `\t`, `\"`, etc.). Non-string arguments are printed in decimal.

- `.error`: immediately halt the second pass with a fatal diagnostic that uses the same argument rules as `.print`. Use it for compile-time validation inside conditionals or macros. Arguments can be strings, numbers, labels, or expressions, separated by commas. When the directive executes the assembler stops and surfaces the concatenated message to the user. Because inactive `.if` blocks are skipped during expansion, `.error` calls inside false branches never trigger:

```
MAX_SIZE = 100

.if (BUFFER_SIZE > MAX_SIZE)
  .error "Buffer size", BUFFER_SIZE, "exceeds", MAX_SIZE
.endif
```

Typical use cases include guardrails for configuration constants, macro argument validation, and short-circuiting builds when a derived value falls outside a legal range.

- `Name = Expr` / `Name EQU Expr`: defines an immutable constant. Both plain and label-style forms are accepted (e.g., `CONST:` followed by `= expr`). The assembler defers evaluating these expressions until after the first pass, so forward references work: you can refer to constants or labels that appear later in the file. If a constant cannot be resolved once all symbols are known, the error points to the exact definition and expression. Reassigning a constant with a different value still triggers an error; use `.var` if you need mutability.

```
OS_FILENAME_LEN_MAX = BASENAME_LEN + BYTE_LEN + EXT_LEN + WORD_LEN
BASENAME_LEN = 8
BYTE_LEN = 1
EXT_LEN = 3
WORD_LEN = 2
```

Local constants: prefix with `@` to give a constant the same scoped resolution as local labels. The assembler picks the most recent `@name` defined at or before the reference within the current scope (file/macro/loop expansion). You can redefine the same local constant later; earlier references keep the earlier value, later references see the new one:

```
CONST1: = $2000
@data_end: = CONST1 * 2   ; emits 0x4000 before end_label
...
end_label:
@data_end: = CONST1 * 4   ; emits 0x8000 after end_label
```

- `.var Name value`: declares a mutable variable whose value can be reassigned later in the file (or inside macros). Unlike `=` or `EQU`, `.var` establishes an initial value but can be updated with either direct assignments (`Counter = Counter - 1`) or a subsequent `EQU`. The symbol participates in all expression contexts just like any other constant.

```
ImmutableConst = 1      ; Initialize constant
            ; Emits: 0x01

Counter .var 10         ; Initialize variable
db Counter              ; Emits: 0x0A

Counter = Counter - 1   ; Update with expression
db Counter              ; Emits: 0x09

Counter equ 5           ; Update with EQU
db Counter              ; Emits: 0x05
```

- `.align value`: pad the output with zero bytes until the program counter reaches the next multiple of `value`, then resume emitting instructions. The argument can be any expression understood by the `.if` evaluator, must be positive, and has to be a power of two (1, 2, 4, 8, ...). If the current address is already aligned no padding is emitted. Example:

- `.storage Length[, Filler]`: reserves `Length` bytes of address space. If `Filler` is provided, the assembler emits that byte `Length` times into the output. If omitted, the bytes are uninitialized in the binary (the PC advances but nothing is written), which is useful for reserving runtime buffers outside the saved ROM image. `Length` and `Filler` both accept full expressions. Example:

```
.org 0x200
buffer:   .storage 16          ; advances PC by 16, writes nothing
table:    .storage 4, 0x7E     ; emits 0x7E 0x7E 0x7E 0x7E
after:    .db 0xAA              ; assembled after the reserved space
```

```
.org $100
Start:
  db 0, 1, 2
.align 16   ; next instructions start at $110
AlignedLabel:
  mvi a, 0
```

`AlignedLabel` is assigned the aligned address ($110) and the gap between `$103` and `$10F` is filled with zeros.

- `.word value[, values...]`: emit one or more 16-bit words at the current address. Values may be decimal, hexadecimal (`0x`/`$`), or binary (`b`/`%`) literals. Negative decimal literals are allowed down to -0x7FFF (15-bit magnitude) and are encoded using two's complement. Each value is written little-endian (low byte first). Example:

```
.word $1234, 42, b0000_1111, -5
```

The snippet above outputs `34 12 2A 00 0F 00 FB FF`.

Alternative: `DW`

- `.dword value[, values...]`: emit one or more 32-bit words at the current address. Values accept the same literal forms as `.word` plus expressions. Negative decimal literals are allowed down to -0x7FFFFFFF (31-bit magnitude) and are encoded using two's complement. Each value is written little-endian (lowest byte first). Example:

```
.dword $12345678, CONST_BASE + 0x22, -1
```

The snippet above outputs `78 56 34 12 22 00 00 01 FF FF FF FF`.

Alternative: `DD`

- `.byte value[, values...]`: emit one or more bytes at the current address. Accepts decimal, hex (`0x`/`$`), or binary (`b`/`%`). Example:

```
.byte 255, $10, 0xFF, b1111_0000, %11_11_00_00
```

The snippet above emits `FF 10 FF F0 F0`.

Alternative: `DB`

- `.macro Name(param, otherParam, optionalParam=$10)`: defines reusable code blocks. A macro's body is copied inline wherever you invoke `Name(...)`, and all parameters are substituted as plain text before the normal two-pass assembly runs. Parameters can be:
  - numeric expressions (constants, labels, arithmetic, shifts, bitwise, etc.)
  - string or character literals in quots
  Defaults apply when an argument is omitted.

Each macro call receives its own namespace for "normal" (`Label:`) and local (`@loop`) labels, so you can safely reuse throwaway labels inside macros or even call a macro recursively. Normal labels defined inside the macro are exported as `MacroName_<call-index>.Label`, letting you jump back into generated code for debugging tricks:

Constants defined inside a macro are also scoped to that macro invocation. The assembler stores them under a per-call namespace, so a `C = 1` inside `MyMacro()` will not overwrite a global `C`, nor will it collide with `C` defined by other macro calls. Each invocation sees its own macro-local constants when evaluating expressions.

```
.macro SetColors(Background=$06, Border=$0e, Addr)
  lda #Background
  sta $d021
  lda #Border
  sta $d020
AddrPtr ldx Addr
  stx $3300
.endmacro

SetColors($0b, $0f, PalettePtr)
SetColors(, MyColor+1, $0000) ; Background uses the default $06
```

```
_LINE_BREAK_ = 106
_PARAG_BREAK_ = 255
_EOD_ = 0
.macro TEXT (string, end_code=_EOD_)
.encoding "screencodecommodore", "mixed"
    .text string
    .byte end_code
.endmacro

TEXT("    Congratulations, hero! You were really", _LINE_BREAK_)
TEXT("good in this epic quest! Time to celebrate", _LINE_BREAK_)
TEXT("with a royal feast of popsicles watching a", _LINE_BREAK_)
TEXT("lowdown on your epic journey below:", _PARAG_BREAK_)
```

Nested macros are supported (up to 32 levels deep), but you cannot open another `.macro` inside a macro body. All macro lines keep their original file/line metadata, so assembler errors still point back to the macro definition.

- `.encoding "Type", "Case"`: selects how upcoming `.text` literals convert characters to bytes. Supported types are `"ascii"` (default) and `"screencodecommodore"`. The optional case argument accepts `"mixed"` (default), `"lower"`, or `"upper"`. Example:

```
.encoding "ascii", "upper"
.text "hello", 'w'           ; emits: 0x48, 0x45, 0x4C, 0x4C, 0x4F, 0x57

.encoding "screencodecommodore"
.text "@AB"                  ; emits: 0x00, 0x01, 0x02
```

---
- `.text value[, values...]`: emits bytes from comma-separated string or character literals using the current `.encoding` settings. Strings honor standard escapes like `\n`, `\t`, `\"`, etc. Example:

```
.encoding "ascii"
.text "   address:   1", '\n', '\0'
; emits: 20 20 20 61 64 64 72 65 73 73 3A 20 20 20 31 0A 00
```