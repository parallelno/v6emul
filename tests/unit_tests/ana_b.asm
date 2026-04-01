; ANA B test - performs AND operation
.org 0x100
    DI
    MVI A, 0x1F
    MVI B, 0x30
    ANA B
    HLT
