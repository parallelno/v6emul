; CMA test - complement accumulator
; Note: Using raw opcode DB 0x2F because the assembler doesn't support CMA mnemonic
.org 0x100
    DI
    MVI A, 0xF0  ; 11110000
    DB 0x2F      ; CMA opcode - complement A -> 00001111 = 0x0F
    HLT
