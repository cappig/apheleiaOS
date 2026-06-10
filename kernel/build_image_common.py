#!/usr/bin/env python3

from __future__ import annotations

import os
import shutil
import stat
import struct
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

SECTOR_SIZE = 512


class BuildError(RuntimeError):
    pass


ROOT_OWNER = (0, 0)
USER_OWNER = (1000, 1000)
JOHN_OWNER = (1001, 1001)


def _meta(mode: int, owner: tuple[int, int] = ROOT_OWNER) -> dict[str, int]:
    uid, gid = owner
    return {"mode": mode, "uid": uid, "gid": gid}


ROOTFS_METADATA_OVERRIDES: dict[str, dict[str, int]] = {
    **{
        rel: _meta(0o755)
        for rel in ("", "bin", "boot", "dev", "etc", "home", "usr", "usr/include", "usr/lib")
    },
    "tmp": _meta(0o1777),
    "home/user": _meta(0o755, USER_OWNER),
    "home/john": _meta(0o755, JOHN_OWNER),
    "etc/passwd": _meta(0o644),
    "etc/group": _meta(0o644),
    "etc/shadow": _meta(0o600),
    "etc/cozette.psf": _meta(0o644),
    "etc/font.psf": _meta(0o644),
    "boot/loader.conf": _meta(0o644),
    "bin/su": _meta(0o4755),
}

ROOTFS_OWNER_PREFIX_OVERRIDES: dict[str, tuple[int, int]] = {
    "home/user": USER_OWNER,
    "home/john": JOHN_OWNER,
}


def div_round_up(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


def align_up(value: int, alignment: int) -> int:
    return div_round_up(value, alignment) * alignment


def write_at(path: Path, offset: int, data: bytes) -> None:
    with path.open("r+b") as f:
        f.seek(offset)
        f.write(data)


def write_file_to_lba(
    dst_image: Path,
    src_file: Path,
    lba: int,
    sector_size: int = SECTOR_SIZE,
) -> None:
    with src_file.open("rb") as src, dst_image.open("r+b") as dst:
        dst.seek(lba * sector_size)
        shutil.copyfileobj(src, dst)


def path_size(path: Path) -> int:
    total = 0
    for root, dirs, files in os.walk(path):
        root_path = Path(root)
        total += 4096
        for d in dirs:
            total += 256 + len(d)
        for f in files:
            p = root_path / f
            total += p.stat().st_size
    return total


def prepare_root_tree(src_root: Path, dst_root: Path) -> None:
    if dst_root.exists():
        shutil.rmtree(dst_root)
    shutil.copytree(src_root, dst_root, symlinks=True)
    (dst_root / "dev").mkdir(parents=True, exist_ok=True)
    (dst_root / "tmp").mkdir(parents=True, exist_ok=True)

    for rel, meta in ROOTFS_METADATA_OVERRIDES.items():
        path = dst_root if not rel else (dst_root / rel)
        if not path.exists():
            continue

        path.chmod(meta["mode"])

        try:
            os.chown(path, meta["uid"], meta["gid"])
        except (PermissionError, OSError):
            pass


def _mbr_entry(status: int, ptype: int, lba: int, sectors: int) -> bytes:
    return struct.pack(
        "<BBBBBBBBII",
        status & 0xFF,
        0xFE,
        0xFF,
        0xFF,
        ptype & 0xFF,
        0xFE,
        0xFF,
        0xFF,
        lba & 0xFFFFFFFF,
        sectors & 0xFFFFFFFF,
    )


def write_mbr(
    image: Path,
    *,
    code440: bytes | None,
    entries: Sequence[tuple[int, int, int, int]],
) -> None:
    if len(entries) > 4:
        raise BuildError("MBR supports at most 4 partition entries")

    mbr = bytearray(SECTOR_SIZE)
    if code440:
        mbr[: min(len(code440), 440)] = code440[:440]

    for i, (status, ptype, lba, sectors) in enumerate(entries):
        off = 446 + i * 16
        mbr[off : off + 16] = _mbr_entry(status, ptype, lba, sectors)

    mbr[510:512] = b"\x55\xAA"
    write_at(image, 0, mbr)


@dataclass
class _Ext2Node:
    name: str
    host_path: Path
    is_dir: bool
    parent: "_Ext2Node | None"
    mode: int
    uid: int
    gid: int
    mtime: int
    inode_num: int = 0
    children: list["_Ext2Node"] = field(default_factory=list)
    content: bytes = b""


def _node_rel_path(node: _Ext2Node) -> str:
    parts: list[str] = []
    current = node

    while current and current.parent is not None:
        if current.name:
            parts.append(current.name)
        current = current.parent

    parts.reverse()
    return "/".join(parts)


def _default_owner_for_rel_path(rel_path: str) -> tuple[int, int]:
    matches = [
        prefix
        for prefix in ROOTFS_OWNER_PREFIX_OVERRIDES
        if rel_path == prefix or rel_path.startswith(prefix + "/")
    ]
    if not matches:
        return ROOT_OWNER
    return ROOTFS_OWNER_PREFIX_OVERRIDES[max(matches, key=len)]


def _scan_tree(path: Path, parent: _Ext2Node | None, name: str) -> _Ext2Node:
    st = path.lstat()
    is_dir = stat.S_ISDIR(st.st_mode)
    if not is_dir and not stat.S_ISREG(st.st_mode):
        raise BuildError(f"unsupported file type in rootfs: {path}")

    node = _Ext2Node(
        name=name,
        host_path=path,
        is_dir=is_dir,
        parent=parent,
        mode=st.st_mode & 0x0FFF,
        uid=st.st_uid & 0xFFFF,
        gid=st.st_gid & 0xFFFF,
        mtime=int(st.st_mtime),
    )

    if is_dir:
        for child_name in sorted(os.listdir(path)):
            child_path = path / child_name
            child = _scan_tree(child_path, node, child_name)
            node.children.append(child)
    else:
        node.content = path.read_bytes()

    return node


def _assign_inode_numbers(root: _Ext2Node) -> dict[int, _Ext2Node]:
    inode_map: dict[int, _Ext2Node] = {}
    root.inode_num = 2
    inode_map[2] = root

    next_inode = 11

    def visit(node: _Ext2Node) -> None:
        nonlocal next_inode
        for child in node.children:
            child.inode_num = next_inode
            inode_map[next_inode] = child
            next_inode += 1
            if child.is_dir:
                visit(child)

    visit(root)
    return inode_map


def _make_dir_content(node: _Ext2Node, block_size: int) -> bytes:
    entries: list[tuple[int, str, int]] = []
    entries.append((node.inode_num, ".", 2))
    parent_inode = node.parent.inode_num if node.parent else node.inode_num
    entries.append((parent_inode, "..", 2))

    for child in node.children:
        dtype = 2 if child.is_dir else 1
        entries.append((child.inode_num, child.name, dtype))

    blocks: list[list[tuple[int, bytes, int, int]]] = []
    current: list[tuple[int, bytes, int, int]] = []
    used = 0

    for inode_num, name, dtype in entries:
        name_b = name.encode("utf-8")
        min_rec = align_up(8 + len(name_b), 4)
        if min_rec > block_size:
            raise BuildError(f"directory entry too large: {name}")

        if current and used + min_rec > block_size:
            blocks.append(current)
            current = []
            used = 0

        current.append((inode_num, name_b, dtype, min_rec))
        used += min_rec

    if current:
        blocks.append(current)

    out = bytearray()
    for block_entries in blocks:
        block = bytearray(block_size)
        pos = 0
        for i, (inode_num, name_b, dtype, min_rec) in enumerate(block_entries):
            rec_len = min_rec
            if i == len(block_entries) - 1:
                rec_len = block_size - pos

            struct.pack_into("<IHBb", block, pos, inode_num, rec_len, len(name_b), dtype)
            block[pos + 8 : pos + 8 + len(name_b)] = name_b
            pos += rec_len

        out += block

    return bytes(out)


def _pack_ext2_inode(
    *,
    inode_mode: int,
    uid: int,
    gid: int,
    size: int,
    mtime: int,
    links: int,
    disk_sectors: int,
    direct_ptrs: Sequence[int],
    indirect_ptrs: Sequence[int],
) -> bytes:
    inode = bytearray(128)
    struct.pack_into("<H", inode, 0, inode_mode & 0xFFFF)
    struct.pack_into("<H", inode, 2, uid & 0xFFFF)
    struct.pack_into("<I", inode, 4, size & 0xFFFFFFFF)
    struct.pack_into("<I", inode, 8, mtime & 0xFFFFFFFF)
    struct.pack_into("<I", inode, 12, mtime & 0xFFFFFFFF)
    struct.pack_into("<I", inode, 16, mtime & 0xFFFFFFFF)
    struct.pack_into("<I", inode, 20, 0)
    struct.pack_into("<H", inode, 24, gid & 0xFFFF)
    struct.pack_into("<H", inode, 26, links & 0xFFFF)
    struct.pack_into("<I", inode, 28, disk_sectors & 0xFFFFFFFF)
    struct.pack_into("<I", inode, 32, 0)
    struct.pack_into("<I", inode, 36, 0)

    for i in range(12):
        val = direct_ptrs[i] if i < len(direct_ptrs) else 0
        struct.pack_into("<I", inode, 40 + i * 4, val & 0xFFFFFFFF)

    for i in range(3):
        val = indirect_ptrs[i] if i < len(indirect_ptrs) else 0
        struct.pack_into("<I", inode, 88 + i * 4, val & 0xFFFFFFFF)

    struct.pack_into("<I", inode, 100, 0)
    struct.pack_into("<I", inode, 104, 0)
    struct.pack_into("<I", inode, 108, (size >> 32) & 0xFFFFFFFF)
    struct.pack_into("<I", inode, 112, 0)
    return bytes(inode)


def build_ext2_image(
    root_tree: Path,
    out_ext2: Path,
    *,
    block_size: int = 4096,
    inode_count: int | None = None,
    growth_numerator: int = 3,
    growth_denominator: int = 2,
    extra_bytes: int = 0,
    minimum_bytes: int = 16 * 1024 * 1024,
) -> None:
    if block_size not in (1024, 2048, 4096):
        raise BuildError("ext2 builder currently supports block sizes 1024/2048/4096")

    root = _scan_tree(root_tree, None, "")
    inode_map = _assign_inode_numbers(root)

    for node in inode_map.values():
        rel_path = _node_rel_path(node)
        default_uid, default_gid = _default_owner_for_rel_path(rel_path)
        node.uid = default_uid
        node.gid = default_gid

        override = ROOTFS_METADATA_OVERRIDES.get(rel_path)
        if not override:
            continue

        node.mode = override.get("mode", node.mode)
        node.uid = override.get("uid", node.uid)
        node.gid = override.get("gid", node.gid)

    for node in inode_map.values():
        if node.is_dir:
            node.content = _make_dir_content(node, block_size)

    min_inodes = max(inode_map.keys()) + 1
    if inode_count is None:
        inode_count = align_up(max(256, min_inodes + 64), 128)
    if inode_count < min_inodes:
        inode_count = align_up(min_inodes, 128)

    inode_size = 128
    inode_table_blocks = div_round_up(inode_count * inode_size, block_size)

    if block_size == 1024:
        superblock_block = 1
    else:
        superblock_block = 0
    gdt_block = superblock_block + 1
    block_bitmap_block = gdt_block + 1
    inode_bitmap_block = block_bitmap_block + 1
    inode_table_block = inode_bitmap_block + 1
    data_start_block = inode_table_block + inode_table_blocks

    root_bytes = max(path_size(root_tree), 1)
    target_bytes = max(
        root_bytes * growth_numerator // growth_denominator,
        root_bytes + extra_bytes,
        minimum_bytes,
    )
    block_count = div_round_up(target_bytes, block_size)

    max_blocks_one_group = block_size * 8
    if block_count > max_blocks_one_group:
        block_count = max_blocks_one_group

    min_blocks = data_start_block + 64
    if block_count < min_blocks:
        block_count = min_blocks

    class Alloc:
        def __init__(self, total_blocks: int):
            self.total_blocks = total_blocks
            self.next_block = data_start_block
            self.used_blocks = set(range(data_start_block))
            self.image = bytearray(total_blocks * block_size)

        def alloc_block(self, payload: bytes = b"") -> int:
            if self.next_block >= self.total_blocks:
                raise BuildError("ext2 image out of space")
            bno = self.next_block
            self.next_block += 1
            self.used_blocks.add(bno)

            start = bno * block_size
            end = start + block_size
            if payload:
                if len(payload) > block_size:
                    raise BuildError("internal ext2 allocation overflow")
                self.image[start : start + len(payload)] = payload
            if len(payload) < block_size:
                self.image[start + len(payload) : end] = b"\x00" * (block_size - len(payload))
            return bno

    def try_build(total_blocks: int) -> bytes:
        alloc = Alloc(total_blocks)
        entries_per_block = block_size // 4

        inode_bytes: dict[int, bytes] = {}

        for ino in sorted(inode_map):
            node = inode_map[ino]
            content = node.content
            data_blocks: list[int] = []

            for off in range(0, len(content), block_size):
                chunk = content[off : off + block_size]
                data_blocks.append(alloc.alloc_block(chunk))

            pointer_blocks = 0

            def alloc_indirect(ptrs: list[int], level: int) -> int:
                nonlocal pointer_blocks
                if not ptrs:
                    return 0

                if level == 1:
                    table = bytearray(block_size)
                    for i, p in enumerate(ptrs):
                        struct.pack_into("<I", table, i * 4, p)
                    pointer_blocks += 1
                    return alloc.alloc_block(bytes(table))

                cap = entries_per_block ** (level - 1)
                children: list[int] = []
                for i in range(0, len(ptrs), cap):
                    child = alloc_indirect(ptrs[i : i + cap], level - 1)
                    children.append(child)

                table = bytearray(block_size)
                for i, p in enumerate(children):
                    struct.pack_into("<I", table, i * 4, p)
                pointer_blocks += 1
                return alloc.alloc_block(bytes(table))

            direct = data_blocks[:12]
            remain = data_blocks[12:]

            indirect_ptrs = [0, 0, 0]
            for level in (1, 2, 3):
                if not remain:
                    break
                cap = entries_per_block ** level
                chunk = remain[:cap]
                remain = remain[cap:]
                indirect_ptrs[level - 1] = alloc_indirect(chunk, level)

            if remain:
                raise BuildError("file too large for ext2 block pointer limits")

            total_inode_blocks = len(data_blocks) + pointer_blocks
            disk_sectors = total_inode_blocks * (block_size // 512)

            if node.is_dir:
                mode = 0x4000 | (node.mode or 0o755)
                links = 2 + sum(1 for c in node.children if c.is_dir)
            else:
                mode = 0x8000 | (node.mode or 0o644)
                links = 1

            inode_bytes[ino] = _pack_ext2_inode(
                inode_mode=mode,
                uid=node.uid,
                gid=node.gid,
                size=len(content),
                mtime=node.mtime,
                links=links,
                disk_sectors=disk_sectors,
                direct_ptrs=direct,
                indirect_ptrs=indirect_ptrs,
            )

        used_inodes = set(range(1, 11)) | set(inode_bytes.keys())
        if max(used_inodes) > inode_count:
            raise BuildError("inode table too small")

        # Metadata tables
        inode_table = bytearray(inode_table_blocks * block_size)
        for ino, raw in inode_bytes.items():
            off = (ino - 1) * inode_size
            inode_table[off : off + len(raw)] = raw

        alloc.image[
            inode_table_block * block_size
            : (inode_table_block + inode_table_blocks) * block_size
        ] = inode_table

        # Bitmaps
        block_bitmap = bytearray(block_size)
        for b in alloc.used_blocks:
            if b >= total_blocks:
                continue
            block_bitmap[b // 8] |= 1 << (b % 8)

        inode_bitmap = bytearray(block_size)
        for ino in used_inodes:
            idx = ino - 1
            if idx >= inode_count:
                continue
            inode_bitmap[idx // 8] |= 1 << (idx % 8)

        alloc.image[
            block_bitmap_block * block_size : (block_bitmap_block + 1) * block_size
        ] = block_bitmap
        alloc.image[
            inode_bitmap_block * block_size : (inode_bitmap_block + 1) * block_size
        ] = inode_bitmap

        free_blocks = total_blocks - len(alloc.used_blocks)
        free_inodes = inode_count - len(used_inodes)
        dir_count = sum(1 for n in inode_map.values() if n.is_dir)

        # Group descriptor
        gd = bytearray(38)
        struct.pack_into("<I", gd, 0, block_bitmap_block)
        struct.pack_into("<I", gd, 4, inode_bitmap_block)
        struct.pack_into("<I", gd, 8, inode_table_block)
        struct.pack_into("<I", gd, 12, free_blocks)
        struct.pack_into("<I", gd, 16, free_inodes)
        struct.pack_into("<I", gd, 20, dir_count)
        gdt_off = gdt_block * block_size
        alloc.image[gdt_off : gdt_off + len(gd)] = gd

        # Superblock at offset 1024
        sb = bytearray(1024)
        now = int(time.time())
        first_data_block = 1 if block_size == 1024 else 0
        bs_shift = {1024: 0, 2048: 1, 4096: 2}[block_size]

        struct.pack_into("<I", sb, 0, inode_count)
        struct.pack_into("<I", sb, 4, total_blocks)
        struct.pack_into("<I", sb, 8, 0)  # reserved blocks
        struct.pack_into("<I", sb, 12, free_blocks)
        struct.pack_into("<I", sb, 16, free_inodes)
        struct.pack_into("<I", sb, 20, first_data_block)
        struct.pack_into("<I", sb, 24, bs_shift)
        struct.pack_into("<I", sb, 28, bs_shift)
        struct.pack_into("<I", sb, 32, total_blocks)
        struct.pack_into("<I", sb, 36, total_blocks)
        struct.pack_into("<I", sb, 40, inode_count)
        struct.pack_into("<I", sb, 44, now)
        struct.pack_into("<I", sb, 48, now)
        struct.pack_into("<H", sb, 52, 0)
        struct.pack_into("<H", sb, 54, 0xFFFF)
        struct.pack_into("<H", sb, 56, 0xEF53)
        struct.pack_into("<H", sb, 58, 1)  # clean
        struct.pack_into("<H", sb, 60, 1)  # ignore errors
        struct.pack_into("<H", sb, 62, 0)
        struct.pack_into("<I", sb, 64, now)
        struct.pack_into("<I", sb, 68, 0)
        struct.pack_into("<I", sb, 72, 0)
        struct.pack_into("<I", sb, 76, 1)  # dynamic rev
        struct.pack_into("<H", sb, 80, 0)
        struct.pack_into("<H", sb, 82, 0)
        struct.pack_into("<I", sb, 84, 11)
        struct.pack_into("<H", sb, 88, inode_size)
        struct.pack_into("<H", sb, 90, 0)
        struct.pack_into("<I", sb, 92, 0)
        struct.pack_into("<I", sb, 96, 0)
        struct.pack_into("<I", sb, 100, 0)
        sb[104:120] = uuid.uuid4().bytes
        sb[120:136] = b"APHELEIA".ljust(16, b"\x00")

        alloc.image[1024 : 1024 + 1024] = sb
        return bytes(alloc.image)

    for _ in range(8):
        try:
            image = try_build(block_count)
            out_ext2.write_bytes(image)
            return
        except BuildError as e:
            if "out of space" not in str(e):
                raise
            step = 32
            new_blocks = min(block_count + step, max_blocks_one_group)
            if new_blocks <= block_count:
                raise
            block_count = new_blocks

    raise BuildError("failed to build ext2 image after retries")
