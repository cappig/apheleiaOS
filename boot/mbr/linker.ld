OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
    . = 0x600;

    .text : {
        *(.mbr)
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note .note.*)
    }
}
