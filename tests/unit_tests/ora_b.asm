; ORA B test - performs OR operation
.org 0x100
    DI
    MVI A, 0x0F
    MVI B, 0x30
    ORA B
    HLT
