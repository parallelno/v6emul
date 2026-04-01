; PUSH/POP test - saves and restores BC register pair
.org 0x100
    DI
    LXI SP, 0x1000  ; Set stack pointer
    LXI B, 0xABCD   ; Load BC with test value
    PUSH B          ; Push BC onto stack
    POP B           ; Pop back into BC
    HLT
