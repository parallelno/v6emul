; STA test - store accumulator to memory
.org 0x100
    DI
    MVI A, 0x77
    STA 0x0200
    HLT
