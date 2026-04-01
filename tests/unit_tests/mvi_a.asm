; MVI A test - loads immediate value into accumulator
.org 0x100
    DI
    MVI A, 0x42
    HLT
