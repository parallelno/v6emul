; JNZ test - jumps when zero flag is not set
.org 0x100
    DI
    MVI A, 0x01     ; A is non-zero, Z flag clear
    JNZ target
    MVI A, 0xAA     ; This should be skipped
    HLT
target:
    MVI A, 0xCC
    HLT
