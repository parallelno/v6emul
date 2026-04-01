; SUB zero test - subtracting same value produces zero
.org 0x100
    DI
    MVI A, 0x42
    MVI B, 0x42
    SUB B
    HLT
