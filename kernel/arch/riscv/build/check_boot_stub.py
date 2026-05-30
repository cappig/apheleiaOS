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
    parser.add_argument("dtb_offset", type=int, nargs="?", default=0)
    parser.add_argument("dtb_path", type=Path, nargs="?")
    args = parser.parse_args()

    footprint = stack_top(args.boot_elf) - IMAGE_BASE
    first_payload = args.rootfs_offset
    if args.dtb_offset:
        first_payload = min(first_payload, args.dtb_offset)

    if footprint > first_payload:
        raise RuntimeError("boot image footprint exceeds first embedded payload offset")

    if args.boot_bin.stat().st_size > first_payload:
        raise RuntimeError("boot binary exceeds first embedded payload offset")

    if args.dtb_offset and args.dtb_offset >= args.rootfs_offset:
        raise RuntimeError("embedded DTB offset must be before embedded rootfs offset")

    if args.dtb_offset and args.dtb_path:
        dtb_end = args.dtb_offset + args.dtb_path.stat().st_size
        if dtb_end > args.rootfs_offset:
            raise RuntimeError("embedded DTB exceeds space before embedded rootfs")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
