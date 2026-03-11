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
    build_ext2_image,
    div_round_up,
    prepare_root_tree,
    write_file_to_lba,
    write_mbr,
)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Create legacy BIOS image (MBR + boot partition + ext2 rootfs)."
    )
    parser.add_argument("output_img", type=Path)
    parser.add_argument("arg1", type=Path)
    parser.add_argument("arg2", type=Path)
    parser.add_argument("arg3", nargs="?", type=Path)
    args = parser.parse_args()

    mbr_bin: Path | None = None
    if args.arg3 is None:
        boot_bin = args.arg1
        rootfs_dir = args.arg2
    else:
        mbr_bin = args.arg1
        boot_bin = args.arg2
        rootfs_dir = args.arg3

    boot_bytes = boot_bin.stat().st_size
    boot_sectors = div_round_up(boot_bytes, SECTOR_SIZE)
    boot_start = 1

    with tempfile.TemporaryDirectory(prefix="apheleia-image-") as td:
        td_path = Path(td)
        root_tree = td_path / "root"
        ext2_img = td_path / "rootfs.ext2"

        prepare_root_tree(rootfs_dir, root_tree)
        build_ext2_image(root_tree, ext2_img, block_size=4096)

        ext2_bytes = ext2_img.stat().st_size
        ext2_sectors = div_round_up(ext2_bytes, SECTOR_SIZE)
        ext2_start = boot_start + boot_sectors

        total_sectors = ext2_start + ext2_sectors
        total_bytes = align_up(total_sectors * SECTOR_SIZE, SECTOR_SIZE)

        with args.output_img.open("wb") as f:
            f.truncate(total_bytes)

        # Partition 1: boot stage (bootable Linux type)
        # Partition 2: ext2 rootfs
        write_mbr(
            args.output_img,
            code440=mbr_bin.read_bytes()[:440] if mbr_bin else None,
            entries=[
                (0x80, 0x83, boot_start, boot_sectors),
                (0x00, 0x83, ext2_start, ext2_sectors),
            ],
        )

        write_file_to_lba(args.output_img, boot_bin, boot_start)
        write_file_to_lba(args.output_img, ext2_img, ext2_start)


if __name__ == "__main__":
    try:
        main()
    except BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
