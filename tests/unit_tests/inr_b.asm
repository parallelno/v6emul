; INR B test - increments register B
.org 0x100
    DI
    MVI B, 0x0F
    INR B
    HLT
