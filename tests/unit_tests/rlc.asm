; RLC test - rotates accumulator left
.org 0x100
    DI
    MVI A, 0x82  ; 10000010 -> 00000101 (bit 7 goes to carry and bit 0)
    RLC
    HLT
