#!/usr/bin/awk -f
# Q&D script that pathches the x86 second stage bootloader location into the MBR

# patch.awk — Patch inode_num and file_blocks into assembled MBR binary
#
# Usage:
#   awk -v inode=1234 -v blocks=56 -f patch.awk mbr.bin > patched.bin

BEGIN {
    # Hardcoded offset where the mbr expects to find these values
    inode_offset = 432
    blocks_offset = inode_offset + 4

    # Convert inode and block count to little endian
    inode_bytes = sprintf("%c%c%c%c",
        inode       & 0xFF,
        (inode>>8)  & 0xFF,
        (inode>>16) & 0xFF,
        (inode>>24) & 0xFF)

    blocks_bytes = sprintf("%c%c",
        blocks      & 0xFF,
        (blocks>>8) & 0xFF)
}

{
    while ((c = getchar()) != -1) {
        pos++

        if (pos == inode_offset + 1) {
            printf "%s", inode_bytes
            for (i=1; i<4; i++)
                c = getchar()

            pos += 3
        } else if (pos == blocks_offset + 1) {
            printf "%s", blocks_bytes
            for (i=1; i<2; i++)
                c = getchar()

            pos += 1
        } else {
            printf "%c", c
        }
    }
}
