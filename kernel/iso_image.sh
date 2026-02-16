#!/bin/bash
set -e

# Usage: ./iso_image.sh <output.iso> <mbr.bin> <bios.bin> <BOOTX64.EFI> <kernel.elf> <rootfs_dir>
#
# Produces a bootable El Torito ISO 9660 image that also works as an HDD image
# (isohybrid). The boot flow depends on the medium type:
#
# CD/DVD boot (El Torito):
#   BIOS firmware reads El Torito boot catalog -> loads bios.bin at 0x7C00
#   (no-emulation) -> stage 2 reads MBR at sector 0 -> finds ext2 partition
#   (type 0x83, non-bootable) -> loads kernel from ext2 rootfs
#
# HDD/USB boot (isohybrid MBR):
#   MBR code runs -> finds bootable partition (entry 0) -> loads bios.bin
#   from its LBA -> stage 2 reads MBR at sector 0 -> finds ext2 partition
#   (type 0x83, non-bootable) -> loads kernel from ext2 rootfs
#
# UEFI boot:
#   El Torito alternate boot entry -> ESP FAT image -> EFI/BOOT/BOOTX64.EFI
#   -> loads kernel from ESP -> kernel mounts ext2 rootfs via ATAPI/ATA + MBR
#
# After xorriso creates the ISO, we patch the isohybrid MBR partition table
# so that entry 0 (bootable) points to bios.bin's actual El Torito LBA,
# allowing the MBR code to load the stage 2 when booted as an HDD.

ISO="$1"
MBR_BIN="$2"
BIOS_BIN="$3"
EFI_APP="$4"
KERNEL_ELF="$5"
ROOTFS="$6"

SECTOR_SIZE=512

ISO_STAGING=$(mktemp -d)
TMP_EXT2=$(mktemp)
TMP_ROOT=$(mktemp -d)
ESP_IMG=""

cleanup() {
    rm -f "$TMP_EXT2" "$ESP_IMG"
    rm -rf "$ISO_STAGING" "$TMP_ROOT"
}
trap cleanup EXIT

# --- Build the ext2 rootfs image ---

cp -a "$ROOTFS"/. "$TMP_ROOT"/
mkdir -p "$TMP_ROOT/dev"

ROOT_SIZE_BYTES=$(du -sb "$TMP_ROOT" | cut -f1)
EXT2_TARGET_BYTES=$((ROOT_SIZE_BYTES * 2))
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
        mkfs.ext2 -q -m 0 -b 1024 -N 2048 -d \"$TMP_ROOT\" \"$TMP_EXT2\"
    "
else
    mkfs.ext2 -q -m 0 -b 1024 -N 2048 -d "$TMP_ROOT" "$TMP_EXT2"
fi

# --- Populate ISO staging directory ---

mkdir -p "$ISO_STAGING/boot"
cp -f "$BIOS_BIN" "$ISO_STAGING/boot/bios.bin"

BIOS_SIZE_BYTES=$(stat -c%s "$BIOS_BIN")
BIOS_LOAD_SECTORS=$(( (BIOS_SIZE_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE ))

# --- Build xorriso arguments ---

XORRISO_ARGS=(
    -as mkisofs
    -o "$ISO"
    -R -J
    -b boot/bios.bin
    -no-emul-boot
    -boot-load-size "$BIOS_LOAD_SECTORS"
)

# Add UEFI boot if we have an EFI application
if [ -n "$EFI_APP" ] && [ -f "$EFI_APP" ]; then
    ESP_IMG=$(mktemp)

    ESP_SIZE_SECTORS=4096   # 2 MiB
    dd if=/dev/zero of="$ESP_IMG" bs=$SECTOR_SIZE count=$ESP_SIZE_SECTORS status=none
    mkfs.fat -F 12 -n "EFI" "$ESP_IMG" >/dev/null

    export MTOOLS_SKIP_CHECK=1
    mmd -i "$ESP_IMG" ::EFI ::EFI/BOOT ::boot
    mcopy -i "$ESP_IMG" -D o "$EFI_APP"    ::EFI/BOOT/BOOTX64.EFI
    mcopy -i "$ESP_IMG" -D o "$KERNEL_ELF" ::boot/kernel64.elf

    cp -f "$ESP_IMG" "$ISO_STAGING/boot/efi.img"

    XORRISO_ARGS+=(
        -eltorito-alt-boot
        -e boot/efi.img
        -no-emul-boot
        -isohybrid-gpt-basdat
    )
fi

XORRISO_ARGS+=(
    -isohybrid-mbr "$MBR_BIN"
    -append_partition 2 0x83 "$TMP_EXT2"
    "$ISO_STAGING"
)

# --- Create the ISO ---

xorriso "${XORRISO_ARGS[@]}" 2>/dev/null

# --- Patch the isohybrid MBR partition table ---
#
# xorriso's -isohybrid-mbr installs our MBR boot code (first 440 bytes)
# but creates its OWN partition table where entry 0 covers the entire
# ISO data area (type 0x00). Our MBR code expects entry 0 to point to
# bios.bin so it can load the stage 2 bootloader.
#
# We extract bios.bin's El Torito LBA (in 2048-byte ISO sectors), convert
# to 512-byte sectors, and rewrite MBR partition entry 0 to point there.

# Extract bios.bin's El Torito LBA from the ISO (2048-byte sectors)
ISO_LBA=$(xorriso -indev "$ISO" -report_el_torito plain 2>&1 \
    | awk '/El Torito boot img :   1/ { print $NF }')

if [ -z "$ISO_LBA" ]; then
    echo "ERROR: could not find bios.bin El Torito LBA" >&2
    exit 1
fi

# Convert ISO 2048-byte sector LBA to 512-byte sector LBA
BIOS_LBA_512=$((ISO_LBA * 4))

# Write a 16-byte MBR partition entry at the given byte offset
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

# Entry 0: bootable, points to bios.bin (stage 2 loader)
# Type 0x83 so the MBR code can find it as a bootable Linux partition
write_mbr_entry "$ISO" 0  0x80 0x83  "$BIOS_LBA_512"  "$BIOS_LOAD_SECTORS"

echo "ISO $ISO"
