; SBB with borrow test - subtracts with carry (borrow)
.org 0x100
    DI
    MVI A, 0x00
    MVI B, 0x01
    SUB B       ; 0x00 - 0x01 = 0xFF, carry (borrow) set
    MVI B, 0xF0
    SBB B       ; 0xFF - 0xF0 - 1 = 0x0E
    HLT
