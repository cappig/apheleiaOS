OUTPUT_ARCH(i386)
/* OUTPUT_FORMAT(elf32-i386) */
ENTRY(_start)

SECTIONS
{
    . = 0x7c00;

    .text : {
        *(.entry)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }
}
