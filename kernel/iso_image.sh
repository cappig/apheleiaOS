#!/bin/bash
set -e

# Creates a bootable El Torito ISO 9660 image (with isohybrid MBR) that
# supports BIOS, and optionally UEFI, boot.
#
# CD/DVD (El Torito):
#   Firmware loads bios.bin via El Torito catalog -> stage 2 reads MBR at
#   sector 0 -> finds ext2 partition -> loads kernel from ext2 rootfs
#
# HDD/USB (isohybrid):
#   MBR code -> loads bios.bin from its El Torito LBA -> same path as above
#
# UEFI (when EFI app is provided):
#   El Torito alternate boot -> ESP FAT image -> EFI/BOOT/BOOTX64.EFI
#   -> loads kernel from ESP -> mounts ext2 rootfs via ATAPI/ATA + MBR
#
# Usage: iso_image.sh <output.iso> <mbr.bin> <bios.bin> [BOOTX64.EFI] [kernel.elf] <rootfs_dir>

ISO="$1"
MBR_BIN="$2"
BIOS_BIN="$3"
EFI_APP="$4"
KERNEL_ELF="$5"
ROOTFS="$6"

SECTOR=512

ISO_STAGING=$(mktemp -d)
TMP_EXT2=$(mktemp)
TMP_ROOT=$(mktemp -d)
ESP_IMG=""

cleanup() {
    rm -f "$TMP_EXT2" "$ESP_IMG"
    rm -rf "$ISO_STAGING" "$TMP_ROOT"
}
trap cleanup EXIT

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

cp -a "$ROOTFS"/. "$TMP_ROOT"/
mkdir -p "$TMP_ROOT/dev"

ROOT_BYTES=$(du -sb "$TMP_ROOT" | cut -f1)
truncate -s $(( ROOT_BYTES * 2 )) "$TMP_EXT2"

if command -v fakeroot >/dev/null 2>&1; then
    fakeroot bash -c "
        chown 0:0       '$TMP_ROOT'/etc/{passwd,group,loader.conf,shadow} 2>/dev/null || true
        chown 0:0       '$TMP_ROOT'/{dev,home} 2>/dev/null || true
        chown 1000:1000 '$TMP_ROOT'/home/user 2>/dev/null || true
        chmod 0644      '$TMP_ROOT'/etc/{passwd,group,loader.conf} 2>/dev/null || true
        chmod 0600      '$TMP_ROOT'/etc/shadow 2>/dev/null || true
        chmod 0755      '$TMP_ROOT'/{dev,home,home/user} 2>/dev/null || true
        mkfs.ext2 -q -m 0 -b 1024 -N 2048 -d '$TMP_ROOT' '$TMP_EXT2'
    "
else
    mkfs.ext2 -q -m 0 -b 1024 -N 2048 -d "$TMP_ROOT" "$TMP_EXT2"
fi

mkdir -p "$ISO_STAGING/boot"
cp -f "$BIOS_BIN" "$ISO_STAGING/boot/bios.bin"

BIOS_BYTES=$(stat -c%s "$BIOS_BIN")
BIOS_LOAD_SECTORS=$(( (BIOS_BYTES + SECTOR - 1) / SECTOR ))

XORRISO_ARGS=(
    -as mkisofs
    -o "$ISO"
    -R -J
    -b boot/bios.bin
    -no-emul-boot
    -boot-load-size "$BIOS_LOAD_SECTORS"
)

# Add UEFI boot support if an EFI application was provided
if [ -n "$EFI_APP" ] && [ -f "$EFI_APP" ]; then
    ESP_IMG=$(mktemp)

    ESP_SECTORS=131072  # 64 MiB
    dd if=/dev/zero of="$ESP_IMG" bs=$SECTOR count=$ESP_SECTORS status=none
    mkfs.fat -F 32 -n "EFI" "$ESP_IMG" >/dev/null

    export MTOOLS_SKIP_CHECK=1
    mmd   -i "$ESP_IMG" ::EFI ::EFI/BOOT ::boot
    mcopy -i "$ESP_IMG" -D o "$EFI_APP"    ::EFI/BOOT/BOOTX64.EFI
    mcopy -i "$ESP_IMG" -D o "$KERNEL_ELF" ::boot/kernel64.elf
    mcopy -i "$ESP_IMG" -D o "$TMP_EXT2"   ::boot/rootfs.ext2

    cp -f "$ESP_IMG" "$ISO_STAGING/boot/efi.img"

    XORRISO_ARGS+=(
        -eltorito-alt-boot
        -e boot/efi.img
        -no-emul-boot
        -isohybrid-gpt-basdat
    )
fi

# Append the ext2 rootfs (and optionally ESP) as extra MBR partitions
# so the BIOS stage-2 can find the root filesystem
if [ -n "$ESP_IMG" ] && [ -f "$ESP_IMG" ]; then
    XORRISO_ARGS+=(
        -isohybrid-mbr "$MBR_BIN"
        -append_partition 2 0x83 "$TMP_EXT2"
        -append_partition 3 0xef "$ESP_IMG"
        "$ISO_STAGING"
    )
else
    XORRISO_ARGS+=(
        -isohybrid-mbr "$MBR_BIN"
        -append_partition 2 0x83 "$TMP_EXT2"
        "$ISO_STAGING"
    )
fi

xorriso "${XORRISO_ARGS[@]}" 2>/dev/null

# Patch isohybrid MBR
ISO_LBA=$(xorriso -indev "$ISO" -report_el_torito plain 2>&1 \
    | awk '/El Torito boot img :   1/ { print $NF }')

if [ -z "$ISO_LBA" ]; then
    echo "error: could not find bios.bin El Torito LBA" >&2
    exit 1
fi

BIOS_LBA_512=$(( ISO_LBA * 4 ))

write_mbr_entry "$ISO" 0  0x80 0x83  "$BIOS_LBA_512"  "$BIOS_LOAD_SECTORS"

echo "ISO $ISO"
