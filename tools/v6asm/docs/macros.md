# Macros

## `.macro` / `.endmacro`

Defines reusable code blocks. A macro's body is copied inline wherever you invoke `Name(...)`, and all parameters are substituted as plain text before the normal two-pass assembly runs.

Parameters can be:
- numeric expressions (constants, labels, arithmetic, shifts, bitwise, etc.)
- string or character literals in quotes

Defaults apply when an argument is omitted.

```asm
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

## Label Scoping

Each macro call receives its own namespace for normal (`Label:`) and local (`@loop`) labels, so you can safely reuse throwaway labels inside macros or even call a macro recursively. Normal labels defined inside the macro are exported as `MacroName_<call-index>.Label`, letting you jump back into generated code for debugging.

## Constant Scoping

Constants defined inside a macro are also scoped to that macro invocation. The assembler stores them under a per-call namespace, so a `C = 1` inside `MyMacro()` will not overwrite a global `C`, nor will it collide with `C` defined by other macro calls. Each invocation sees its own macro-local constants when evaluating expressions.

## Nesting

Nested macros are supported (up to 32 levels deep), but you cannot open another `.macro` inside a macro body. All macro lines keep their original file/line metadata, so assembler errors still point back to the macro definition.

## Example with Text Encoding

```asm
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
