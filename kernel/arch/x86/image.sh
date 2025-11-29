#!/bin/bash
set -e

# Usage: ./image.sh.sh <image.bin> <boot.bin> <rootfs>

IMG="$1"
BOOT_BIN="$2"
ROOTFS="$3"

SECTOR_SIZE=512
BOOT_START=1

# Calculate bootloader size
BOOT_SIZE_BYTES=$(stat -c%s "$BOOT_BIN")
BOOT_SIZE_SECTORS=$(((BOOT_SIZE_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE))

# Create ext2 rootfs
TMP_EXT2=$(mktemp)
truncate -s 16M "$TMP_EXT2"

mkfs.ext2 -q -b 1024 -d "$ROOTFS" "$TMP_EXT2"

resize2fs -M "$TMP_EXT2" >/dev/null 2>&1

EXT2_SIZE_BYTES=$(stat -c%s "$TMP_EXT2")
EXT2_SECTORS=$(((EXT2_SIZE_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE))

EXT2_START=$((BOOT_START + BOOT_SIZE_SECTORS))
TOTAL_SECTORS=$((EXT2_START + EXT2_SECTORS))

# Create disk image
dd if=/dev/zero of="$IMG" bs=$SECTOR_SIZE count=$TOTAL_SECTORS status=none

# Create partition table
sfdisk "$IMG" >/dev/null <<EOF
$BOOT_START,$BOOT_SIZE_SECTORS,0x83,*
$EXT2_START,$EXT2_SECTORS,0x83
EOF

dd if="$BOOT_BIN" of="$IMG" bs=$SECTOR_SIZE seek=$BOOT_START conv=notrunc status=none
dd if="$TMP_EXT2" of="$IMG" bs=$SECTOR_SIZE seek=$EXT2_START conv=notrunc status=none

rm "$TMP_EXT2"

echo "Image $IMG created successfully"
