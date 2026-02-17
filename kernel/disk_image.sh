#!/bin/bash
set -e

# Creates a hybrid MBR/GPT disk image bootable on both BIOS and UEFI.
#
# Layout:
#   Sector 0        Hybrid MBR
#   Sectors 1-33    GPT header + entries
#   Partition 1     BIOS boot (flat stage-2 binary, type EF02)
#   Partition 2     EFI System Partition (FAT32, type EF00)
#   Partition 3     ext2 root filesystem (type 8300)
#
# BIOS path:  MBR -> stage 2 (BIOS boot partition) -> ext2 -> kernel
# UEFI path:  GPT -> ESP -> EFI/BOOT/BOOTX64.EFI -> kernel from ESP
#
# Usage: disk_image.sh <output.img> <mbr.bin> <bios.bin> <BOOTX64.EFI> <kernel.elf> <rootfs_dir>

IMG="$1"
MBR_BIN="$2"
BIOS_BIN="$3"
EFI_APP="$4"
KERNEL_ELF="$5"
ROOTFS="$6"

SECTOR=512
ALIGN=2048

ESP_IMG=$(mktemp)
TMP_EXT2=$(mktemp)
TMP_ROOT=$(mktemp -d)
trap 'rm -f "$ESP_IMG" "$TMP_EXT2"; rm -rf "$TMP_ROOT"' EXIT

# Writes a 16-byte MBR partition entry at the given index (0-3).
write_mbr_entry() {
    local img="$1" idx="$2" status="$3" type="$4" lba="$5" sectors="$6"
    local off=$(( 446 + idx * 16 ))

    local entry=""
    entry+=$(printf '\\x%02x' "$status")
    entry+='\xfe\xff\xff'
    entry+=$(printf '\\x%02x' "$type")
    entry+='\xfe\xff\xff'
    entry+=$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
        $(( lba & 0xFF )) $(( (lba >> 8) & 0xFF )) \
        $(( (lba >> 16) & 0xFF )) $(( (lba >> 24) & 0xFF )))
    entry+=$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
        $(( sectors & 0xFF )) $(( (sectors >> 8) & 0xFF )) \
        $(( (sectors >> 16) & 0xFF )) $(( (sectors >> 24) & 0xFF )))

    printf "$entry" | dd of="$img" bs=1 seek="$off" conv=notrunc status=none
}

# BIOS bootloader

BIOS_BYTES=$(stat -c%s "$BIOS_BIN")
BIOS_SECTORS=$(( (BIOS_BYTES + SECTOR - 1) / SECTOR ))

# EFI System Partition (FAT32, 64 MiB)
ESP_SECTORS=131072
dd if=/dev/zero of="$ESP_IMG" bs=$SECTOR count=$ESP_SECTORS status=none
mkfs.fat -F 32 -n "EFI" "$ESP_IMG" >/dev/null

export MTOOLS_SKIP_CHECK=1
mmd   -i "$ESP_IMG" ::EFI ::EFI/BOOT ::boot
mcopy -i "$ESP_IMG" -D o "$EFI_APP"    ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" -D o "$KERNEL_ELF" ::boot/kernel64.elf

# Root filesystem
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

# Copy rootfs into ESP so the UEFI kernel can find it
mcopy -i "$ESP_IMG" -D o "$TMP_EXT2" ::boot/rootfs.ext2

GPT_FOOTER=33

BIOS_START=$ALIGN
BIOS_END=$(( BIOS_START + BIOS_SECTORS - 1 ))

ESP_START=$(( ((BIOS_END + ALIGN) / ALIGN) * ALIGN ))
ESP_END=$(( ESP_START + ESP_SECTORS - 1 ))

EXT2_START=$(( ((ESP_END + ALIGN) / ALIGN) * ALIGN ))
EXT2_END=$(( EXT2_START + EXT2_SECTORS - 1 ))

TOTAL=$(( EXT2_END + 1 + GPT_FOOTER ))

dd if=/dev/zero of="$IMG" bs=$SECTOR count=$TOTAL status=none

sgdisk --clear \
    --new=1:${BIOS_START}:${BIOS_END}  --typecode=1:EF02 --change-name=1:"BIOS Boot" \
    --new=2:${ESP_START}:${ESP_END}     --typecode=2:EF00 --change-name=2:"EFI System" \
    --new=3:${EXT2_START}:${EXT2_END}   --typecode=3:8300 --change-name=3:"apheleiaOS" \
    "$IMG" >/dev/null

dd if="$BIOS_BIN" of="$IMG" bs=$SECTOR seek=$BIOS_START conv=notrunc status=none
dd if="$ESP_IMG"  of="$IMG" bs=$SECTOR seek=$ESP_START  conv=notrunc status=none
dd if="$TMP_EXT2" of="$IMG" bs=$SECTOR seek=$EXT2_START conv=notrunc status=none

write_mbr_entry "$IMG" 0  0x00 0xEF  $ESP_START   $ESP_SECTORS
write_mbr_entry "$IMG" 1  0x80 0x83  $BIOS_START  $BIOS_SECTORS
write_mbr_entry "$IMG" 2  0x00 0x83  $EXT2_START  $EXT2_SECTORS
write_mbr_entry "$IMG" 3  0x00 0xEE  1            $(( TOTAL - 1 ))

# Install MBR boot code
dd if="$MBR_BIN" of="$IMG" bs=1 count=440 conv=notrunc status=none
