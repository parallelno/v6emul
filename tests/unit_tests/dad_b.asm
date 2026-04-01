; DAD B test - adds BC to HL
.org 0x100
    DI
    LXI H, 0x1111
    LXI B, 0x1111
    DAD B
    HLT
