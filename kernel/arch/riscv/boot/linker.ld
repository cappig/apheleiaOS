OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text_segment PT_LOAD FLAGS(5); /* R_E */
    data_segment PT_LOAD FLAGS(6); /* RW_ */
}

SECTIONS
{
    . = 0x80000000;
    PROVIDE(__image_start = .);

    .text : ALIGN(16) {
        KEEP(*(.init))
        *(.text .text.*)
    } : text_segment

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } : text_segment

    .data : ALIGN(16) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } : data_segment

    PROVIDE(__image_end = .);

    PROVIDE(__global_pointer$ = . + 0x800);

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } : data_segment

    .boot_heap : ALIGN(16) {
        __heap_start = .;
        . += 1024 * 1024;
        __heap_end = .;
    } : data_segment

    . = ALIGN(16);
    . += 64 * 1024;
    __stack_top = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }
}
