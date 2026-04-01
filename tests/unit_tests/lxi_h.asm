; LXI H test - loads 16-bit immediate into HL
.org 0x100
    DI
    LXI H, 0x1234
    HLT
