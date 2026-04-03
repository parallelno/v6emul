# Listing File Format

The assembler can generate a `.lst` listing file alongside the ROM output when the `--lst` flag is passed. The listing interleaves assembled machine code with the original source, making it easy to verify addresses, encodings, and macro/loop expansions.

## Generating a Listing

```bash
v6asm main.asm --lst
```

This produces `main.lst` next to the ROM output.

## Column Layout

Each line in the listing follows a fixed-width columnar format:

```
ADDR   BYTES                    SOURCE
```

| Column | Width | Description |
|--------|-------|-------------|
| `ADDR` | 4 hex digits | The 16-bit memory address where the bytes are placed. Blank for lines that emit no bytes (comments, labels-only, directives like `.org`). |
| `BYTES` | Up to 24 characters | Hex dump of the emitted bytes, space-separated. At most 8 bytes are shown per line. |
| `SOURCE` | Variable | A right-aligned line number (from the source file) followed by the original source text. |

The columns are separated by fixed whitespace: 3 spaces between ADDR and BYTES, then the source column follows.

## Address Column

- Displayed as a 4-digit uppercase hex value (e.g., `0100`, `110D`).
- Present only when the line produces output bytes or is a `.storage` directive.
- Blank (4 spaces) for lines that produce no machine code, such as comments, label-only lines, constant definitions, and `.org`.

## Bytes Column

- Each byte is shown as a 2-digit uppercase hex value, space-separated (e.g., `21 00 00`).
- A maximum of **8 bytes** are displayed per line.
- When a line emits **more than 8 bytes**, only the first 8 are shown and a `+` suffix is appended to indicate truncation:
  ```
  010D   FF FF FF FF FF FF FF FF+    15  			.storage 0x1000, 0xff
  ```
- Blank (24 spaces) for lines that emit no bytes.

## Source Column

- A **line number** (1-indexed, right-aligned in a 5-character field) from the original source file.
- Followed by 2 spaces and the **original source text** exactly as written.

## Included Files

When a `.include` directive pulls in another file, the listing shows the included content with line numbers **reset to 1** for that file. This makes it easy to identify which file each line originated from:

```
                                   48  	  		hlt
                                   49
                                    1  PALETTE_LEN = 16+1+2+3+0b100_000_000-$100-0x00
                                    2  set_palette: ; non-local label
1143   21 74 11                     3  			lxi h, palette + PALETTE_LEN - 1
```

Line 49 is from the main file; the numbering resets to 1 when the included file begins.

## Loop Expansions

`.loop` blocks are expanded inline. The loop directive itself produces no bytes. Each iteration of the loop body appears as a separate listing line, all sharing the **same source line number** from the original `.loop` body but with incrementing addresses:

```
                                   62  		.loop 7
11B0   76                          62  			hlt
11B1   76                          62  			hlt
11B2   76                          62  			hlt
11B3   76                          62  			hlt
11B4   76                          62  			hlt
11B5   76                          62  			hlt
11B6   76                          62  			hlt
```

All seven `hlt` instructions share line number 62 from the source, with addresses advancing from `$11B0` through `$11B6`.

## Macro Expansions

Macro invocations are expanded similarly to loops. The listing shows each expanded instruction with the address and bytes it produces, using the line numbers from the macro definition body.

## Full Example

```
ADDR   BYTES                    SOURCE
                                    1  ; set the palette using direct port I/O
                                    2  			OPCODE_EI = 0xFB
                                    3
                                    4  .org 0x100
                                    5  start:
0100   21 00 00                     6  			lxi h, 0x00
0103   22 0A 01                     7  			shld @test1 + 1
0106   C3 0D 11                     8  			jmp @main
                                    9  @test1:
0109   16 11 FF                    10  			.byte PALETTE_LEN, 0x11, 0xff
010C   10                          11  			.byte PALETTE_LEN2
                                   12  @test_data:
010D   FF FF FF FF FF FF FF FF+    13  			.storage 0x1000, 0xff
```

Key observations:
- Lines 1–4 produce no bytes, so the ADDR column is blank.
- Line 5 (`start:`) is a label-only line — no address or bytes.
- Lines 6–8 show 3-byte instructions with addresses advancing from `$0100`.
- Line 13 uses `.storage` to emit `0x1000` bytes; only the first 8 are shown, with `+` indicating more follow.
