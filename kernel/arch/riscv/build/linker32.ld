OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text_segment PT_LOAD FLAGS(5); /* R_E */
    data_segment PT_LOAD FLAGS(6); /* RW_ */
}

SECTIONS
{
    . = 0x81400000;

    .text : ALIGN(16) {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } : text_segment

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    } : text_segment

    .data : ALIGN(16) {
        *(.data .data.*)
    } : data_segment

    PROVIDE(__global_pointer$ = . + 0x800);

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        . += (64 * 1024);
        __stack_top = .;
        __bss_end = .;
    } : data_segment

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }
}
