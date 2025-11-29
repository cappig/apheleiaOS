#!/bin/bash

# Usage: ./mbr.sh <mbr.bin> <disk.img>
# burn the first 446 bytes to the mbr

set -e

MBR_BIN="$1"
DISK_IMG="$2"

# Check size of mbr.bin
MBR_SIZE=$(stat -c%s "$MBR_BIN")
if [ "$MBR_SIZE" -gt 446 ]; then
    echo "mbr.bin too big! Must be <= 446 bytes."
    exit 1
fi

# Overwrite first 446 bytes of disk image
dd if="$MBR_BIN" of="$DISK_IMG" bs=1 count="$MBR_SIZE" conv=notrunc
echo "MBR stub written"
