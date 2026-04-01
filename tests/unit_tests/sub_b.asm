; SUB B test - subtracts register B from accumulator
.org 0x100
    DI
    MVI A, 0x10
    MVI B, 0x0B
    SUB B
    HLT
