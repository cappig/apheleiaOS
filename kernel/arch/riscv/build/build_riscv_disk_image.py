#!/usr/bin/env python3

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

KERNEL_DIR = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(KERNEL_DIR))

from build_image_common import (
    BuildError,
    SECTOR_SIZE,
    align_up,
    build_ext2_image,
    prepare_root_tree,
    write_at,
    write_file_to_lba,
)

ENTRY_ALIGN = 4096

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Create RISC-V image (boot entry + ext2 rootfs)."
    )
    parser.add_argument("output_img", type=Path)
    parser.add_argument("boot_entry", type=Path)
    parser.add_argument("rootfs_dir", type=Path)
    args = parser.parse_args()

    entry_bytes = args.boot_entry.read_bytes()
    entry_size = len(entry_bytes)
    ext2_offset = align_up(entry_size, ENTRY_ALIGN)

    with tempfile.TemporaryDirectory(prefix="apheleia-image-") as td:
        td_path = Path(td)
        root_tree = td_path / "root"
        ext2_img = td_path / "rootfs.ext2"

        prepare_root_tree(args.rootfs_dir, root_tree)
        build_ext2_image(root_tree, ext2_img, block_size=4096)

        ext2_bytes = ext2_img.stat().st_size
        total_bytes = align_up(ext2_offset + ext2_bytes, SECTOR_SIZE)

        with args.output_img.open("wb") as f:
            f.truncate(total_bytes)

        write_at(args.output_img, 0, entry_bytes)
        write_file_to_lba(args.output_img, ext2_img, ext2_offset // SECTOR_SIZE)

if __name__ == "__main__":
    try:
        main()
    except BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
