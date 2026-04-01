; JZ test - does not jump when zero flag is not set
.org 0x100
    DI
    MVI A, 0x01     ; A is non-zero, Z flag clear
    JZ skip         ; Should NOT jump
    MVI A, 0xDD     ; This should execute
    HLT
skip:
    NOP
    HLT
