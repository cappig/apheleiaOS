#!/usr/bin/env python3

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

from build_image_common import (
    BuildError,
    SECTOR_SIZE,
    align_up,
    build_esp_fat16_image,
    build_ext2_image,
    div_round_up,
    prepare_root_tree,
    write_file_to_lba,
    write_gpt,
    write_mbr,
)

ALIGN_SECTORS = 2048
ESP_SECTORS = 131072  # 64 MiB
GPT_FOOTER_SECTORS = 33

GPT_BIOS_BOOT_GUID = "21686148-6449-6E6F-744E-656564454649"
GPT_ESP_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
GPT_LINUX_GUID = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Create hybrid BIOS+UEFI disk image with GPT + hybrid MBR."
    )
    parser.add_argument("output_img", type=Path)
    parser.add_argument("mbr_bin", type=Path)
    parser.add_argument("bios_bin", type=Path)
    parser.add_argument("efi_app", type=Path)
    parser.add_argument("kernel_elf", type=Path)
    parser.add_argument("rootfs_dir", type=Path)
    args = parser.parse_args()

    bios_bytes = args.bios_bin.stat().st_size
    bios_sectors = div_round_up(bios_bytes, SECTOR_SIZE)

    with tempfile.TemporaryDirectory(prefix="apheleia-disk-") as td:
        td_path = Path(td)
        root_tree = td_path / "root"
        ext2_img = td_path / "rootfs.ext2"
        esp_img = td_path / "esp.img"

        prepare_root_tree(args.rootfs_dir, root_tree)
        build_ext2_image(root_tree, ext2_img, block_size=1024)
        build_esp_fat16_image(
            esp_img,
            size_sectors=ESP_SECTORS,
            efi_app=args.efi_app,
            kernel_elf=args.kernel_elf,
            rootfs_image=ext2_img,
        )

        ext2_bytes = ext2_img.stat().st_size
        ext2_sectors = div_round_up(ext2_bytes, SECTOR_SIZE)

        bios_start = ALIGN_SECTORS
        bios_end = bios_start + bios_sectors - 1

        esp_start = align_up(bios_end + 1, ALIGN_SECTORS)
        esp_end = esp_start + ESP_SECTORS - 1

        ext2_start = align_up(esp_end + 1, ALIGN_SECTORS)
        ext2_end = ext2_start + ext2_sectors - 1

        total_sectors = ext2_end + 1 + GPT_FOOTER_SECTORS
        total_bytes = total_sectors * SECTOR_SIZE

        with args.output_img.open("wb") as f:
            f.truncate(total_bytes)

        write_gpt(
            args.output_img,
            total_sectors=total_sectors,
            partitions=[
                {
                    "type_guid": GPT_BIOS_BOOT_GUID,
                    "name": "BIOS Boot",
                    "start_lba": bios_start,
                    "end_lba": bios_end,
                },
                {
                    "type_guid": GPT_ESP_GUID,
                    "name": "EFI System",
                    "start_lba": esp_start,
                    "end_lba": esp_end,
                },
                {
                    "type_guid": GPT_LINUX_GUID,
                    "name": "apheleiaOS",
                    "start_lba": ext2_start,
                    "end_lba": ext2_end,
                },
            ],
        )

        write_file_to_lba(args.output_img, args.bios_bin, bios_start)
        write_file_to_lba(args.output_img, esp_img, esp_start)
        write_file_to_lba(args.output_img, ext2_img, ext2_start)

        mbr_code = args.mbr_bin.read_bytes()[:440]

        # Hybrid MBR for BIOS compatibility while retaining GPT for UEFI.
        write_mbr(
            args.output_img,
            code440=mbr_code,
            entries=[
                (0x00, 0xEF, esp_start, ESP_SECTORS),
                (0x80, 0x83, bios_start, bios_sectors),
                (0x00, 0x83, ext2_start, ext2_sectors),
                (0x00, 0xEE, 1, total_sectors - 1),
            ],
        )


if __name__ == "__main__":
    try:
        main()
    except BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
