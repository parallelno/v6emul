; INX H test - increment HL register pair
.org 0x100
    DI
    LXI H, 0x0FFF
    INX H       ; 0x0FFF + 1 = 0x1000
    HLT
