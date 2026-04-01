; CALL/RET test - subroutine call and return
.org 0x100
    DI
    LXI SP, 0x1000  ; Set stack pointer
    CALL subr       ; Call subroutine
    JMP done        ; Continue after return
subr:
    MVI A, 0xEE
    RET
done:
    NOP
    HLT
