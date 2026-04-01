; DCX H test - decrement HL register pair
.org 0x100
    DI
    LXI H, 0x1000
    DCX H       ; 0x1000 - 1 = 0x0FFF
    HLT
