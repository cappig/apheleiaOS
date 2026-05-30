#!/usr/bin/env python3
"""Build a RISC-V flat image and patch its embedded layout header."""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path


PAGE_SIZE = 4096
IMAGE_MAGIC = 0x52564941
IMAGE_VERSION = 1
HEADER_STRUCT = struct.Struct("<IHHQQQQ")


def align(value: int, size: int = PAGE_SIZE) -> int:
    return (value + size - 1) & ~(size - 1)


def symbol_addr(elf: Path, name: str) -> int:
    out = subprocess.check_output(["readelf", "-Ws", str(elf)], text=True)

    for line in out.splitlines():
        fields = line.split()
        if fields and fields[-1] == name:
            return int(fields[1], 16)

    raise RuntimeError(f"{name} not found in {elf}")


def load_segments(elf: Path) -> list[tuple[int, int]]:
    out = subprocess.check_output(["readelf", "-W", "-l", str(elf)], text=True)
    segs: list[tuple[int, int]] = []

    for line in out.splitlines():
        fields = line.split()
        if fields[:1] == ["LOAD"] and len(fields) >= 6:
            segs.append((int(fields[3], 16), int(fields[5], 16)))

    if not segs:
        raise RuntimeError(f"no LOAD segments found in {elf}")

    return segs


def check_kernel_layout(
    kernel_elf: Path,
    image_base: int,
    rootfs_offset: int,
    rootfs_size: int,
    scratch_offset: int,
) -> None:
    segs = load_segments(kernel_elf)

    kernel_start = min(addr for addr, _ in segs)
    kernel_end = max(addr + size for addr, size in segs)

    rootfs_start = image_base + rootfs_offset
    rootfs_end = rootfs_start + rootfs_size
    scratch_start = image_base + scratch_offset if scratch_offset else 0

    if kernel_end > rootfs_start and kernel_start < rootfs_end:
        raise RuntimeError(
            f"kernel [{kernel_start:#x}, {kernel_end:#x}) overlaps "
            f"rootfs [{rootfs_start:#x}, {rootfs_end:#x})"
        )

    if scratch_start and rootfs_end > scratch_start:
        raise RuntimeError(
            f"rootfs [{rootfs_start:#x}, {rootfs_end:#x}) overlaps "
            f"boot scratch [{scratch_start:#x}, ...)"
        )

    if scratch_start and kernel_end > scratch_start:
        raise RuntimeError(
            f"kernel [{kernel_start:#x}, {kernel_end:#x}) overlaps "
            f"boot scratch [{scratch_start:#x}, ...)"
        )


def place(image: bytearray, offset: int, payload: bytes, name: str) -> None:
    if len(image) > offset:
        raise RuntimeError(f"{name} offset overlaps earlier image data")

    image.extend(b"\0" * (offset - len(image)))
    image.extend(payload)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build a RISC-V flat boot image.")
    parser.add_argument("--boot-elf", type=Path, required=True)
    parser.add_argument("--boot-bin", type=Path, required=True)
    parser.add_argument("--kernel-elf", type=Path, required=True)
    parser.add_argument("--rootfs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scratch-offset", type=int, default=0)
    parser.add_argument("--dtb", type=Path)
    args = parser.parse_args()

    image_base = symbol_addr(args.boot_elf, "__image_start")
    stack_top = symbol_addr(args.boot_elf, "__stack_top")
    header_addr = symbol_addr(args.boot_elf, "riscv_image_header")

    header_offset = header_addr - image_base
    if header_offset < 0:
        raise RuntimeError("image header is before image base")

    boot = bytearray(args.boot_bin.read_bytes())
    rootfs = args.rootfs.read_bytes()
    dtb = args.dtb.read_bytes() if args.dtb else b""

    boot_footprint = stack_top - image_base
    first_payload = align(max(len(boot), boot_footprint))

    dtb_offset = 0
    if dtb:
        dtb_offset = first_payload
        rootfs_offset = align(dtb_offset + len(dtb))
    else:
        rootfs_offset = first_payload

    header_end = header_offset + HEADER_STRUCT.size
    if header_end > len(boot):
        raise RuntimeError("image header is outside boot binary")

    check_kernel_layout(args.kernel_elf, image_base, rootfs_offset, len(rootfs), args.scratch_offset)

    header = HEADER_STRUCT.pack(
        IMAGE_MAGIC,
        IMAGE_VERSION,
        HEADER_STRUCT.size,
        dtb_offset,
        len(dtb),
        rootfs_offset,
        len(rootfs),
    )

    boot[header_offset:header_end] = header

    image = bytearray(boot)
    if dtb:
        place(image, dtb_offset, dtb, "DTB")

    place(image, rootfs_offset, rootfs, "rootfs")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
