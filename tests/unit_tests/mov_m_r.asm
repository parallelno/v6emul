; MOV M,r test - store register to memory via HL
.org 0x100
    DI
    LXI H, 0x0200   ; Point HL to memory location
    MVI A, 0x88     ; Load value into A
    MOV M, A        ; Store A to memory at (HL)
    HLT
