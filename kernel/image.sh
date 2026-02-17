#!/bin/bash
set -e

# Creates a legacy MBR disk image with two partitions:
#   Partition 1: bootloader (flat binary, marked bootable)
#   Partition 2: ext2 root filesystem
#
# Usage: image.sh <output.img> <boot.bin> <rootfs_dir>

IMG="$1"
BOOT_BIN="$2"
ROOTFS="$3"

SECTOR=512
BOOT_START=1

BOOT_BYTES=$(stat -c%s "$BOOT_BIN")
BOOT_SECTORS=$(( (BOOT_BYTES + SECTOR - 1) / SECTOR ))

TMP_EXT2=$(mktemp)
TMP_ROOT=$(mktemp -d)
trap 'rm -f "$TMP_EXT2"; rm -rf "$TMP_ROOT"' EXIT

cp -a "$ROOTFS"/. "$TMP_ROOT"/
mkdir -p "$TMP_ROOT/dev"

ROOT_BYTES=$(du -sb "$TMP_ROOT" | cut -f1)
truncate -s $(( ROOT_BYTES + ROOT_BYTES / 2 )) "$TMP_EXT2"

if command -v fakeroot >/dev/null 2>&1; then
    fakeroot bash -c "
        chown 0:0       '$TMP_ROOT'/etc/{passwd,group,loader.conf,shadow} 2>/dev/null || true
        chown 0:0       '$TMP_ROOT'/{dev,home} 2>/dev/null || true
        chown 1000:1000 '$TMP_ROOT'/home/user 2>/dev/null || true
        chmod 0644      '$TMP_ROOT'/etc/{passwd,group,loader.conf} 2>/dev/null || true
        chmod 0600      '$TMP_ROOT'/etc/shadow 2>/dev/null || true
        chmod 0755      '$TMP_ROOT'/{dev,home,home/user} 2>/dev/null || true
        mkfs.ext2 -q -m 0 -b 1024 -d '$TMP_ROOT' '$TMP_EXT2'
    "
else
    mkfs.ext2 -q -m 0 -b 1024 -d "$TMP_ROOT" "$TMP_EXT2"
fi

EXT2_BYTES=$(stat -c%s "$TMP_EXT2")
EXT2_SECTORS=$(( (EXT2_BYTES + SECTOR - 1) / SECTOR ))

EXT2_START=$(( BOOT_START + BOOT_SECTORS ))
TOTAL=$(( EXT2_START + EXT2_SECTORS ))

dd if=/dev/zero of="$IMG" bs=$SECTOR count=$TOTAL status=none

sfdisk "$IMG" >/dev/null <<EOF
${BOOT_START},${BOOT_SECTORS},0x83,*
${EXT2_START},${EXT2_SECTORS},0x83
EOF

dd if="$BOOT_BIN" of="$IMG" bs=$SECTOR seek=$BOOT_START conv=notrunc status=none
dd if="$TMP_EXT2" of="$IMG" bs=$SECTOR seek=$EXT2_START conv=notrunc status=none
