; RRC test - rotates accumulator right
.org 0x100
    DI
    MVI A, 0x83  ; 10000011 -> 11000001 (bit 0 goes to carry and bit 7)
    RRC
    HLT
