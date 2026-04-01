; XRA B test - performs XOR operation
.org 0x100
    DI
    MVI A, 0x1F
    MVI B, 0x30
    XRA B
    HLT
