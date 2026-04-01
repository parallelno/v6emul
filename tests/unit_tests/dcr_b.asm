; DCR B test - decrements register B
.org 0x100
    DI
    MVI B, 0x0F
    DCR B
    HLT
