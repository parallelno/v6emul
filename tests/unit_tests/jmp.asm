; JMP test - unconditional jump
.org 0x100
    DI
    JMP skip
    MVI A, 0xAA     ; This should be skipped
    HLT
skip:
    MVI A, 0xBB
    HLT
