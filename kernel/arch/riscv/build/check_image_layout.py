#!/usr/bin/env python3
"""Verify that kernel ELF load segments don't overlap the embedded rootfs."""

import os
import subprocess
import sys


def load_segments(elf_path):
    out = subprocess.check_output(["readelf", "-W", "-l", elf_path], text=True)
    segs = []
    for line in out.splitlines():
        parts = line.split()
        if parts[:1] == ["LOAD"] and len(parts) >= 6:
            segs.append((int(parts[3], 16), int(parts[5], 16)))
    return segs


def main():
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <kernel.elf> <rootfs.img> <rootfs_offset>")
        sys.exit(1)

    kernel_elf = sys.argv[1]
    rootfs_img = sys.argv[2]
    rootfs_off = int(sys.argv[3])

    segs = load_segments(kernel_elf)
    if not segs:
        print("error: no LOAD segments found in kernel ELF", file=sys.stderr)
        sys.exit(1)

    kernel_start = min(addr for addr, _ in segs)
    kernel_end = max(addr + size for addr, size in segs)

    rootfs_start = 0x80000000 + rootfs_off
    rootfs_end = rootfs_start + os.path.getsize(rootfs_img)

    if kernel_end > rootfs_start and kernel_start < rootfs_end:
        print(
            f"error: kernel [{kernel_start:#x}, {kernel_end:#x}) overlaps "
            f"rootfs [{rootfs_start:#x}, {rootfs_end:#x})",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
