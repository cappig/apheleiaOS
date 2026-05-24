#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

IMAGE_BASE = 0x80000000


def stack_top(boot_elf: Path) -> int:
    out = subprocess.check_output(["readelf", "-Ws", str(boot_elf)], text=True)

    for line in out.splitlines():
        fields = line.split()
        if fields and fields[-1] == "__stack_top":
            return int(fields[1], 16)

    raise RuntimeError("__stack_top not found")


def main() -> None:
    parser = argparse.ArgumentParser(description="Check RISC-V boot-stub size.")
    parser.add_argument("boot_elf", type=Path)
    parser.add_argument("boot_bin", type=Path)
    parser.add_argument("rootfs_offset", type=int)
    args = parser.parse_args()

    footprint = stack_top(args.boot_elf) - IMAGE_BASE
    if footprint > args.rootfs_offset:
        raise RuntimeError("boot image footprint exceeds embedded rootfs offset")

    if args.boot_bin.stat().st_size > args.rootfs_offset:
        raise RuntimeError("boot binary exceeds embedded rootfs offset")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
