; CMP B test - compares A with B, sets flags but doesn't modify A
.org 0x100
    DI
    MVI A, 0x20
    MVI B, 0x10
    CMP B
    HLT
