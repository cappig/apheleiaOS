OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* X + R */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;            /* R */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* W + R */
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text : ALIGN(4K) {
        *(.bootstrap)
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4K)  {
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(4K) {
        *(.data .data.*)
    } :data

    .bss : ALIGN(4K) {
        *(.bss .bss.*)
        *(COMMON)
    } :data


    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
    }
}
