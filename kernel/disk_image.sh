#!/bin/bash
set -e

# Usage: ./disk_image.sh <image.img> <mbr.bin> <bios.bin> <BOOTX64.EFI> <kernel.elf> <rootfs_dir>
#
# Produces a hybrid MBR/GPT disk image that boots on both BIOS and UEFI
# firmware, in any virtual machine or on bare metal hardware.
#
# Disk layout:
#   Sector 0:      Hybrid MBR (custom boot code + partition table)
#   Sectors 1-33:  GPT header + entries
#   Sector 2048:   BIOS boot partition (flat stage 2 binary, GPT type EF02)
#   Sector 4096:   EFI System Partition (FAT, GPT type EF00)
#   Sector 8192:   ext2 root filesystem (GPT type 8300)
#
# BIOS boot path:
#   MBR code -> loads stage 2 from BIOS boot partition -> finds ext2 root
#   via MBR partition table -> loads kernel from /boot/kernel.elf
#
# UEFI boot path:
#   GPT -> ESP -> EFI/BOOT/BOOTX64.EFI -> loads kernel from ESP
#   -> kernel mounts ext2 root via GPT

IMG="$1"
MBR_BIN="$2"
BIOS_BIN="$3"
EFI_APP="$4"
KERNEL_ELF="$5"
ROOTFS="$6"

SECTOR_SIZE=512
ALIGN=2048

ESP_IMG=$(mktemp)
TMP_EXT2=$(mktemp)
TMP_ROOT=$(mktemp -d)
cleanup() { rm -f "$ESP_IMG" "$TMP_EXT2"; rm -rf "$TMP_ROOT"; }
trap cleanup EXIT

# Write a 16-byte MBR partition entry at the given index (0-3).
write_mbr_entry() {
    local img="$1" idx="$2" status="$3" type="$4" lba="$5" sectors="$6"
    local base=$((446 + idx * 16))

    local b=""
    b+=$(printf '\\x%02x' "$status")
    b+='\xfe\xff\xff'
    b+=$(printf '\\x%02x' "$type")
    b+='\xfe\xff\xff'
    b+=$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
        $((lba & 0xFF)) $(((lba >> 8) & 0xFF)) \
        $(((lba >> 16) & 0xFF)) $(((lba >> 24) & 0xFF)))
    b+=$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
        $((sectors & 0xFF)) $(((sectors >> 8) & 0xFF)) \
        $(((sectors >> 16) & 0xFF)) $(((sectors >> 24) & 0xFF)))

    printf "$b" | dd of="$img" bs=1 seek="$base" conv=notrunc status=none
}

BIOS_SIZE_BYTES=$(stat -c%s "$BIOS_BIN")
BIOS_SIZE_SECTORS=$(((BIOS_SIZE_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE))

ESP_SIZE_SECTORS=4096   # 2 MiB

dd if=/dev/zero of="$ESP_IMG" bs=$SECTOR_SIZE count=$ESP_SIZE_SECTORS status=none
mkfs.fat -F 12 -n "EFI" "$ESP_IMG" >/dev/null

export MTOOLS_SKIP_CHECK=1
mmd -i "$ESP_IMG" ::EFI ::EFI/BOOT ::boot
mcopy -i "$ESP_IMG" -D o "$EFI_APP"    ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" -D o "$KERNEL_ELF" ::boot/kernel64.elf

cp -a "$ROOTFS"/. "$TMP_ROOT"/
mkdir -p "$TMP_ROOT/dev"

ROOT_SIZE_BYTES=$(du -sb "$TMP_ROOT" | cut -f1)
EXT2_TARGET_BYTES=$((ROOT_SIZE_BYTES + ROOT_SIZE_BYTES / 2))
truncate -s "$EXT2_TARGET_BYTES" "$TMP_EXT2"

if command -v fakeroot >/dev/null 2>&1; then
    fakeroot sh -c "
        chown 0:0 \"$TMP_ROOT/etc/passwd\" \"$TMP_ROOT/etc/group\" \"$TMP_ROOT/etc/loader.conf\" \"$TMP_ROOT/etc/shadow\" 2>/dev/null || true
        chown 0:0 \"$TMP_ROOT/dev\" 2>/dev/null || true
        chown 0:0 \"$TMP_ROOT/home\" 2>/dev/null || true
        chown 1000:1000 \"$TMP_ROOT/home/user\" 2>/dev/null || true
        chmod 0644 \"$TMP_ROOT/etc/passwd\" \"$TMP_ROOT/etc/group\" \"$TMP_ROOT/etc/loader.conf\" 2>/dev/null || true
        chmod 0600 \"$TMP_ROOT/etc/shadow\" 2>/dev/null || true
        chmod 0755 \"$TMP_ROOT/dev\" 2>/dev/null || true
        chmod 0755 \"$TMP_ROOT/home\" \"$TMP_ROOT/home/user\" 2>/dev/null || true
        mkfs.ext2 -q -m 0 -b 1024 -d \"$TMP_ROOT\" \"$TMP_EXT2\"
    "
else
    mkfs.ext2 -q -m 0 -b 1024 -d "$TMP_ROOT" "$TMP_EXT2"
fi

EXT2_SIZE_BYTES=$(stat -c%s "$TMP_EXT2")
EXT2_SECTORS=$(((EXT2_SIZE_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE))

GPT_FOOTER_SECTORS=33

BIOS_START=$ALIGN
BIOS_END=$((BIOS_START + BIOS_SIZE_SECTORS - 1))

ESP_START=$(( ((BIOS_END + ALIGN) / ALIGN) * ALIGN ))
ESP_END=$((ESP_START + ESP_SIZE_SECTORS - 1))

EXT2_START=$(( ((ESP_END + ALIGN) / ALIGN) * ALIGN ))
EXT2_END=$((EXT2_START + EXT2_SECTORS - 1))

TOTAL_SECTORS=$((EXT2_END + 1 + GPT_FOOTER_SECTORS))

dd if=/dev/zero of="$IMG" bs=$SECTOR_SIZE count=$TOTAL_SECTORS status=none

sgdisk --clear \
    --new=1:${BIOS_START}:${BIOS_END}   --typecode=1:EF02 --change-name=1:"BIOS Boot" \
    --new=2:${ESP_START}:${ESP_END}      --typecode=2:EF00 --change-name=2:"EFI System" \
    --new=3:${EXT2_START}:${EXT2_END}    --typecode=3:8300 --change-name=3:"apheleiaOS" \
    "$IMG" >/dev/null

dd if="$BIOS_BIN" of="$IMG" bs=$SECTOR_SIZE seek=$BIOS_START  conv=notrunc status=none
dd if="$ESP_IMG"   of="$IMG" bs=$SECTOR_SIZE seek=$ESP_START   conv=notrunc status=none
dd if="$TMP_EXT2"  of="$IMG" bs=$SECTOR_SIZE seek=$EXT2_START  conv=notrunc status=none

# Overwrite the protective MBR's partition table with hybrid entries.
# Entry 0: GPT protective (0xEE) covering the gap before the first partition
# Entry 1: BIOS boot partition (bootable 0x80, type 0x83)
# Entry 2: EFI System Partition (type 0xEF)
# Entry 3: ext2 root filesystem (type 0x83, non-bootable)

write_mbr_entry "$IMG" 0  0x00 0xEE  1              $((BIOS_START - 1))
write_mbr_entry "$IMG" 1  0x80 0x83  $BIOS_START    $BIOS_SIZE_SECTORS
write_mbr_entry "$IMG" 2  0x00 0xEF  $ESP_START     $ESP_SIZE_SECTORS
write_mbr_entry "$IMG" 3  0x00 0x83  $EXT2_START    $EXT2_SECTORS

# Write MBR boot code (first 440 bytes only, preserving disk signature)
dd if="$MBR_BIN" of="$IMG" bs=1 count=440 conv=notrunc status=none
