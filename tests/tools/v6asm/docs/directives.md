# Directives

All directive names are **case-insensitive**: `.org`, `.ORG`, and `.Org` are equivalent.

## `.org`

Sets the program counter to a specific address. Accepts decimal, `0x..`, or `$..` literals.

```asm
.org 0x100
```

## `.include`

Include another file inline.

```asm
.include "file.asm"
.include 'file.asm'
```

Paths are resolved relative to the including file, the main asm file, or the workspace directory. Includes support recursive expansion up to 16 levels.

## `.filesize`

Defines a constant equal to the byte size of a given file. The path resolves like `.include` (current file, main asm, project folder, workspace, then CWD).

```asm
ROM_SIZE .filesize "out/prg.rom" ; ROM_SIZE becomes the size of prg.rom
BufEnd   = BufStart + ROM_SIZE    ; use it in expressions
```

## `.incbin`

Include raw bytes from an external file at the current address.

```asm
.incbin "path"              ; include entire file
.incbin "path", offset      ; start at offset
.incbin "path", offset, length  ; start at offset, read length bytes
```

Paths resolve like `.include`. `offset` and `length` are optional expressions (decimal, hex, or binary); omit them to start at 0 and read the entire file.

## `.if` / `.endif`

Conditional assembly. The block assembles only when the expression evaluates to non-zero. Nesting is supported, and the parser short-circuits inactive branches so forward references inside skipped blocks do not trigger errors.

The argument may be a single numeric/boolean literal or any full expression. Expressions support decimal/hex/binary literals, character constants, symbol names (labels, constants, `@local` labels), arithmetic (`+ - * / % << >>`), comparisons (`== != < <= > >=`), bitwise logic (`& | ^ ~`), and boolean operators (`! && ||`).

```asm
Value = 3

.if (Value >= 2) && (SomeFlag == TRUE)
  mvi a, #$00
  sta $d020
.endif
```

## `.loop` / `.endloop`

Repeat a block of source lines a given number of times (maximum: 100,000 per loop). Short form: `.endl`.

Loop counts are evaluated with the same expression engine as `.if`. Loop bodies can nest other `.loop` or `.if` blocks. Constant assignments inside the block execute on each iteration because the assembler expands the body inline:

```asm
Value = 0
Step  = 4

.loop (Step / 2)
  db Value
  Value = Value + 1
.endl
```

The example above emits `Value` three times (0, 1, 2) and leaves `Value` set to 3 for subsequent code.

## `.optional` / `.endoptional`

Defines an optional code block that is automatically removed from output if none of its internal labels and constants are used externally. Short forms: `.opt` / `.endopt`.

```asm
.optional
useless_byte:
  db 0       ; removed if useless_byte is never referenced
.endoptional
```

```asm
call useful_routine
.opt
useful_routine:
  mvi a, 1 ; kept because useful_routine label was used
  ret      ; kept because useful_routine label was used
.endopt
```

## `.function` / `.endfunction`

This is an alias for `.optional` / `.endoptional`. Short forms: `.func` / `.endfunc`

## `.setting`

Updates assembler defaults using non-case-sensitive key/value pairs. Values may be string, integer, or boolean. Multiple pairs can be specified in one directive.

```asm
.setting optional, false ; disables pruning of .optional blocks
```

## Labels and Constants

### Global Labels

Standard labels defined with a trailing colon. They are visible throughout the entire file and across included files.

### Local Labels and Constants (`@name`)

Locals are scoped between the nearest surrounding global labels (or the start/end of the file/macro/loop expansion). A reference resolves to the closest definition in the same scope, preferring the latest definition at or before the reference; if none, it falls back to the next definition in that scope. Locals are per-file/per-macro and do not collide with globals.

```asm
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

### Constants (`=` / `EQU`)

Defines an immutable constant. Both plain and label-style forms are accepted (e.g., `CONST:` followed by `= expr`). The assembler defers evaluating these expressions until after the first pass, so forward references work. Reassigning a constant with a different value triggers an error; use `.var` for mutability.

```asm
OS_FILENAME_LEN_MAX = BASENAME_LEN + BYTE_LEN + EXT_LEN + WORD_LEN
BASENAME_LEN = 8
BYTE_LEN = 1
EXT_LEN = 3
WORD_LEN = 2
```

Local constants: prefix with `@` to give a constant the same scoped resolution as local labels:

```asm
CONST1: = $2000
@data_end: = CONST1 * 2   ; emits 0x4000 before end_label
...
end_label:
@data_end: = CONST1 * 4   ; emits 0x8000 after end_label
```

### Mutable Variables (`.var`)

Declares a mutable variable whose value can be reassigned later. Unlike `=` or `EQU`, `.var` establishes an initial value but can be updated with direct assignments or a subsequent `EQU`.

```asm
ImmutableConst = 1      ; Initialize constant
            ; Emits: 0x01

Counter .var 10         ; Initialize variable
db Counter              ; Emits: 0x0A

Counter = Counter - 1   ; Update with expression
db Counter              ; Emits: 0x09

Counter equ 5           ; Update with EQU
db Counter              ; Emits: 0x05
```

## `.print`

Emit compile-time diagnostics to the console during the second pass. Arguments are comma-separated and can mix string literals, numeric literals, labels, or arbitrary expressions.

```asm
.print "Copying from", SourceAddr, "to", DestAddr
.print "Loop count:", (EndAddr - StartAddr) / 16
```

Strings honor standard escapes (`\n`, `\t`, `\"`, etc.). Non-string arguments are printed in decimal.

## `.error`

Immediately halt the second pass with a fatal diagnostic. Uses the same argument rules as `.print`. Because inactive `.if` blocks are skipped, `.error` calls inside false branches never trigger.

```asm
MAX_SIZE = 100

.if (BUFFER_SIZE > MAX_SIZE)
  .error "Buffer size", BUFFER_SIZE, "exceeds", MAX_SIZE
.endif
```

## `.align`

Pad the output with zero bytes until the program counter reaches the next multiple of the given value. The argument must be positive and a power of two. If already aligned, no padding is emitted.

```asm
.org $100
Start:
  db 0, 1, 2
.align 16   ; next instructions start at $110
AlignedLabel:
  mvi a, 0
```

`AlignedLabel` is assigned the aligned address ($110) and the gap between `$103` and `$10F` is filled with zeros.

## `.storage`

Reserves bytes of address space. If `Filler` is provided, the assembler emits that byte `Length` times. If omitted, the bytes are uninitialized (PC advances but nothing is written), useful for runtime buffers.

```asm
.org 0x200
buffer:   .storage 16          ; advances PC by 16, writes nothing
table:    .storage 4, 0x7E     ; emits 0x7E 0x7E 0x7E 0x7E
after:    .db 0xAA              ; assembled after the reserved space
```

## Data Emission

### `.byte` / `DB`

Emit one or more bytes at the current address. Accepts decimal, hex (`0x`/`$`), or binary (`b`/`%`).

```asm
.byte 255, $10, 0xFF, b1111_0000, %11_11_00_00
```

Emits `FF 10 FF F0 F0`.

### `.word` / `DW`

Emit one or more 16-bit words (little-endian). Negative values down to -0x7FFF are encoded using two's complement.

```asm
.word $1234, 42, b0000_1111, -5
```

Outputs `34 12 2A 00 0F 00 FB FF`.

### `.dword` / `DD`

Emit one or more 32-bit words (little-endian). Negative values down to -0x7FFFFFFF are encoded using two's complement.

```asm
.dword $12345678, CONST_BASE + 0x22, -1
```

Outputs `78 56 34 12 22 00 00 01 FF FF FF FF`.

## `.encoding`

Selects how upcoming `.text` literals convert characters to bytes. Supported types are `"ascii"` (default) and `"screencodecommodore"`. The optional case argument accepts `"mixed"` (default), `"lower"`, or `"upper"`.

```asm
.encoding "ascii", "upper"
.text "hello", 'w'           ; emits: 0x48, 0x45, 0x4C, 0x4C, 0x4F, 0x57

.encoding "screencodecommodore"
.text "@AB"                  ; emits: 0x00, 0x01, 0x02
```

## `.text`

Emits bytes from comma-separated string or character literals using the current `.encoding` settings. Strings honor standard escapes like `\n`, `\t`, `\"`, etc.

```asm
.encoding "ascii"
.text "   address:   1", '\n', '\0'
; emits: 20 20 20 61 64 64 72 65 73 73 3A 20 20 20 31 0A 00
```
