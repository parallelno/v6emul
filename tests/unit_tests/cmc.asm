; CMC test - complement carry flag
.org 0x100
    DI
    STC     ; Set carry flag
    CMC     ; Complement it (clear)
    HLT
