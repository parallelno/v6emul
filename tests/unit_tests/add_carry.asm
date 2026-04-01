; ADD with carry test - adds 0x80 + 0x80 to produce overflow
.org 0x100
    DI
    MVI A, 0x80
    MVI B, 0x80
    ADD B
    HLT
