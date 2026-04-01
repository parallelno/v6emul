; ADC with carry test - adds with carry flag set
.org 0x100
    DI
    MVI A, 0x80
    MVI B, 0x80
    ADD B       ; 0x80 + 0x80 = 0x00, carry set
    MVI B, 0x10
    ADC B       ; 0x00 + 0x10 + carry = 0x11
    HLT
