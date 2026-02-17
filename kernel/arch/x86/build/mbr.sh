#!/bin/bash
set -e

# Burns the MBR boot stub (up to 446 bytes) onto a disk image,
# preserving the existing partition table and boot signature.
#
# Usage: mbr.sh <mbr.bin> <disk.img>

MBR_BIN="$1"
DISK_IMG="$2"

MBR_SIZE=$(stat -c%s "$MBR_BIN")
if [ "$MBR_SIZE" -gt 446 ]; then
    echo "error: mbr.bin is $MBR_SIZE bytes (max 446)" >&2
    exit 1
fi

dd if="$MBR_BIN" of="$DISK_IMG" bs=1 count="$MBR_SIZE" conv=notrunc status=none
