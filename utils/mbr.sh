#!/bin/bash

# Simple patch script for Makefile use
# Usage: ./mbr.sh <disk.img> <mbr.bin> <image_dir>

set -e

DISK_IMG="$1"
MBR_BIN="$2"
IMAGE_DIR="$3"

# Hardcoded offset where the mbr expects to find these values
INODE_OFFSET=432
BLOCKS_OFFSET=436

# Calculate filesystem size
FS_SIZE=$(du -sb "$IMAGE_DIR" | awk '{blocks = int(($1+1023)/1024); print blocks < 60 ? 60 : blocks}')

# Create the ext2 filesystem
mke2fs -q -t ext2 -b 1024 -d "$IMAGE_DIR" "$DISK_IMG" "$FS_SIZE"

# Extract inode and size
BIOS_STAT=$(debugfs -R "stat boot/bios.bin" "$DISK_IMG" 2>/dev/null)
INODE=$(echo "$BIOS_STAT" | awk '/Inode:/ {print $2}')

# Convert to little endian
inode_bytes=$(printf "%08x" "$INODE" | sed 's/\(..\)/\1 /g' | awk '{for(i=4;i>0;i--)printf "\\x%s",$i}')

# Patch binary
dd of="$DISK_IMG" if="$MBR_BIN" bs=512 count=1 conv=notrunc status=none
printf "$inode_bytes" | dd of="$DISK_IMG" bs=1 seek=$INODE_OFFSET count=4 conv=notrunc status=none

echo "patched mbr with inode=$INODE"
