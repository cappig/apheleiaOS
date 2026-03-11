OUTPUT_ARCH(i386)
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

PHDRS
{
    text_segment PT_LOAD FLAGS(5); /* R_E */
    data_segment PT_LOAD FLAGS(6); /* RW_ */
}

SECTIONS
{
    . = 0x7c00;

    .text : {
        *(.entry)
        *(.text .text.*)
    } : text_segment

    .rodata : {
        *(.rodata .rodata.*)
    } : text_segment

    .data : {
        *(.data .data.*)
    } : data_segment

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } : data_segment

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }
}
