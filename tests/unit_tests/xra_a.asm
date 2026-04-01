; XRA A test - XOR A with itself clears A and sets zero flag
.org 0x100
    DI
    MVI A, 0xFF
    XRA A
    HLT
