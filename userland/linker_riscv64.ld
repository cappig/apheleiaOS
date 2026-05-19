OUTPUT_FORMAT(elf64-littleriscv)
OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ;
    rodata  PT_LOAD    FLAGS((1 << 2)) ;
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ;
}

SECTIONS
{
    . = 0x00400000;

    .text : ALIGN(4K) {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4K) {
        *(.srodata .srodata.*)
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(4K) {
        *(.sdata .sdata.*)
        *(.data .data.*)
    } :data

    PROVIDE(__global_pointer$ = . + 0x800);

    .bss : ALIGN(4K) {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(.scommon)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }
}
