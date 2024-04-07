section .text

global _gdt_desc
_gdt_desc:
.size:  dw _gdt.end - _gdt - 1
.addr:  dd _gdt

global _gdt
_gdt:
.null: ; 0x00
    dq 0
.realmode_code: ; 0x08
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0b10011011
    db 0b00001111
    db 0x00
.realmode_data: ; 0x10
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0b10010011
    db 0b00001111
    db 0x00
.protmode_code: ; 0x18
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0b10011011
    db 0b11001111
    db 0x00
.protmode_data: ; 0x20
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0b10010011
    db 0b11001111
    db 0x00
.end:
