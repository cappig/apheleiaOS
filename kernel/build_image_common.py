#!/usr/bin/env python3

from __future__ import annotations

import os
import shutil
import stat
import struct
import time
import uuid
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

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
    **{rel: _meta(0o755) for rel in ("", "bin", "boot", "dev", "etc", "home")},
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


def write_gpt(
    image: Path,
    *,
    total_sectors: int,
    partitions: Sequence[dict[str, object]],
    disk_guid: uuid.UUID | None = None,
    entry_count: int = 128,
    entry_size: int = 128,
) -> None:
    if entry_size < 128 or entry_size % 8 != 0:
        raise BuildError("invalid GPT entry size")

    disk_guid = disk_guid or uuid.uuid4()

    entries_blob = bytearray(entry_count * entry_size)
    for i, part in enumerate(partitions):
        if i >= entry_count:
            raise BuildError("too many GPT partitions")

        off = i * entry_size
        ptype = uuid.UUID(str(part["type_guid"]))
        puid = uuid.uuid4()
        start = int(part["start_lba"])
        end = int(part["end_lba"])
        attrs = int(part.get("attrs", 0))
        name = str(part.get("name", "")).encode("utf-16le")[:72]

        entries_blob[off : off + 16] = ptype.bytes_le
        entries_blob[off + 16 : off + 32] = puid.bytes_le
        struct.pack_into("<QQQ", entries_blob, off + 32, start, end, attrs)
        entries_blob[off + 56 : off + 56 + len(name)] = name

    entries_crc = zlib.crc32(entries_blob) & 0xFFFFFFFF

    primary_entries_lba = 2
    backup_entries_lba = total_sectors - 33
    first_usable = 34
    last_usable = total_sectors - 34

    def build_header(current_lba: int, backup_lba: int, entries_lba: int) -> bytes:
        hdr = bytearray(SECTOR_SIZE)
        hdr[0:8] = b"EFI PART"
        struct.pack_into("<I", hdr, 8, 0x00010000)
        struct.pack_into("<I", hdr, 12, 92)
        struct.pack_into("<I", hdr, 16, 0)
        struct.pack_into("<I", hdr, 20, 0)
        struct.pack_into("<Q", hdr, 24, current_lba)
        struct.pack_into("<Q", hdr, 32, backup_lba)
        struct.pack_into("<Q", hdr, 40, first_usable)
        struct.pack_into("<Q", hdr, 48, last_usable)
        hdr[56:72] = disk_guid.bytes_le
        struct.pack_into("<Q", hdr, 72, entries_lba)
        struct.pack_into("<I", hdr, 80, entry_count)
        struct.pack_into("<I", hdr, 84, entry_size)
        struct.pack_into("<I", hdr, 88, entries_crc)
        crc = zlib.crc32(hdr[:92]) & 0xFFFFFFFF
        struct.pack_into("<I", hdr, 16, crc)
        return bytes(hdr)

    write_at(image, primary_entries_lba * SECTOR_SIZE, bytes(entries_blob))
    write_at(image, backup_entries_lba * SECTOR_SIZE, bytes(entries_blob))
    write_at(image, 1 * SECTOR_SIZE, build_header(1, total_sectors - 1, primary_entries_lba))
    write_at(
        image,
        (total_sectors - 1) * SECTOR_SIZE,
        build_header(total_sectors - 1, 1, backup_entries_lba),
    )


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
    target_bytes = max(root_bytes * growth_numerator // growth_denominator, minimum_bytes)
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
            new_blocks = min(block_count * 2, max_blocks_one_group)
            if new_blocks <= block_count:
                raise
            block_count = new_blocks

    raise BuildError("failed to build ext2 image after retries")


def _entry_name_83(name: str) -> tuple[bytes, bytes]:
    if "." in name:
        stem, ext = name.rsplit(".", 1)
    else:
        stem, ext = name, ""

    stem = stem.upper()
    ext = ext.upper()
    if not stem or len(stem) > 8 or len(ext) > 3:
        raise BuildError(f"name is not 8.3 compatible: {name}")
    if not all(c.isalnum() or c in "_-$~!#%&{}()@'^`" for c in stem + ext):
        raise BuildError(f"name has unsupported chars for 8.3: {name}")

    return stem.encode("ascii").ljust(8, b" "), ext.encode("ascii").ljust(3, b" ")


def _dir_entry_raw(name11: bytes, attr: int, first_cluster: int, size: int = 0) -> bytes:
    if len(name11) != 11:
        raise BuildError("invalid directory entry name length")
    e = bytearray(32)
    e[0:11] = name11
    e[11] = attr & 0xFF
    struct.pack_into("<H", e, 26, first_cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size & 0xFFFFFFFF)
    return bytes(e)


def _dir_entry(name: str, attr: int, first_cluster: int, size: int = 0) -> bytes:
    n, x = _entry_name_83(name)
    return _dir_entry_raw(n + x, attr, first_cluster, size)


def build_esp_fat16_image(
    out_path: Path,
    *,
    size_sectors: int,
    efi_app: Path,
    kernel_elf: Path,
    rootfs_image: Path,
) -> None:
    files = {
        ("EFI", "BOOT", "BOOTX64.EFI"): efi_app.read_bytes(),
        ("BOOT", "KERNEL64.ELF"): kernel_elf.read_bytes(),
        ("BOOT", "ROOTFS.IMG"): rootfs_image.read_bytes(),
    }

    bps = SECTOR_SIZE
    spc = 4
    reserved = 1
    num_fats = 2
    root_entries = 512
    root_dir_sectors = div_round_up(root_entries * 32, bps)

    spf = 1
    while True:
        data_sectors = size_sectors - (reserved + num_fats * spf + root_dir_sectors)
        cluster_count = data_sectors // spc
        need = div_round_up((cluster_count + 2) * 2, bps)
        if need == spf:
            break
        spf = need

    data_sectors = size_sectors - (reserved + num_fats * spf + root_dir_sectors)
    cluster_count = data_sectors // spc
    if cluster_count < 32 or cluster_count >= 65525:
        raise BuildError("FAT16 layout invalid for selected ESP size")

    cluster_size = spc * bps
    fat = [0] * (cluster_count + 2)
    fat[0] = 0xFFF8
    fat[1] = 0xFFFF
    next_cluster = 2

    def alloc_clusters(count: int) -> list[int]:
        nonlocal next_cluster
        if count <= 0:
            return []
        start = next_cluster
        end = start + count
        if end > len(fat):
            raise BuildError("ESP FAT image out of space")
        next_cluster = end
        return list(range(start, end))

    c_efi = alloc_clusters(1)[0]
    c_boot = alloc_clusters(1)[0]
    c_efi_boot = alloc_clusters(1)[0]
    fat[c_efi] = 0xFFFF
    fat[c_boot] = 0xFFFF
    fat[c_efi_boot] = 0xFFFF

    file_chains: dict[tuple[str, ...], list[int]] = {}
    for path_key, blob in files.items():
        count = max(1, div_round_up(len(blob), cluster_size))
        chain = alloc_clusters(count)
        file_chains[path_key] = chain
        for i, cluster in enumerate(chain):
            fat[cluster] = chain[i + 1] if i + 1 < len(chain) else 0xFFFF

    image = bytearray(size_sectors * bps)

    bs = bytearray(bps)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"MSDOS5.0"
    struct.pack_into("<H", bs, 11, bps)
    bs[13] = spc
    struct.pack_into("<H", bs, 14, reserved)
    bs[16] = num_fats
    struct.pack_into("<H", bs, 17, root_entries)
    struct.pack_into("<H", bs, 19, 0)
    bs[21] = 0xF8
    struct.pack_into("<H", bs, 22, spf)
    struct.pack_into("<H", bs, 24, 63)
    struct.pack_into("<H", bs, 26, 255)
    struct.pack_into("<I", bs, 28, 0)
    struct.pack_into("<I", bs, 32, size_sectors)
    bs[36] = 0x80
    bs[38] = 0x29
    struct.pack_into("<I", bs, 39, 0xA041EA11)
    bs[43:54] = b"EFI        "
    bs[54:62] = b"FAT16   "
    bs[510:512] = b"\x55\xAA"
    image[0:bps] = bs

    fat_blob = bytearray(spf * bps)
    for i, value in enumerate(fat):
        off = i * 2
        if off + 2 > len(fat_blob):
            break
        struct.pack_into("<H", fat_blob, off, value & 0xFFFF)

    fat1_off = reserved * bps
    fat2_off = (reserved + spf) * bps
    image[fat1_off : fat1_off + len(fat_blob)] = fat_blob
    image[fat2_off : fat2_off + len(fat_blob)] = fat_blob

    root_dir_sector = reserved + num_fats * spf
    root_dir_off = root_dir_sector * bps
    data_sector = root_dir_sector + root_dir_sectors

    def cluster_off(cluster: int) -> int:
        return (data_sector + (cluster - 2) * spc) * bps

    def write_dir(cluster: int, entries: Iterable[bytes]) -> None:
        blob = b"".join(entries)
        if len(blob) > cluster_size:
            raise BuildError("directory exceeds cluster size")
        off = cluster_off(cluster)
        image[off : off + len(blob)] = blob

    root_entries_blob = (
        _dir_entry("EFI", 0x10, c_efi)
        + _dir_entry("BOOT", 0x10, c_boot)
        + b"\x00" * 32
    )
    image[root_dir_off : root_dir_off + len(root_entries_blob)] = root_entries_blob

    write_dir(
        c_efi,
        [
            _dir_entry_raw(b".          ", 0x10, c_efi),
            _dir_entry_raw(b"..         ", 0x10, 0),
            _dir_entry("BOOT", 0x10, c_efi_boot),
            b"\x00" * 32,
        ],
    )
    write_dir(
        c_efi_boot,
        [
            _dir_entry_raw(b".          ", 0x10, c_efi_boot),
            _dir_entry_raw(b"..         ", 0x10, c_efi),
            _dir_entry(
                "BOOTX64.EFI",
                0x20,
                file_chains[("EFI", "BOOT", "BOOTX64.EFI")][0],
                len(files[("EFI", "BOOT", "BOOTX64.EFI")]),
            ),
            b"\x00" * 32,
        ],
    )
    write_dir(
        c_boot,
        [
            _dir_entry_raw(b".          ", 0x10, c_boot),
            _dir_entry_raw(b"..         ", 0x10, 0),
            _dir_entry(
                "KERNEL64.ELF",
                0x20,
                file_chains[("BOOT", "KERNEL64.ELF")][0],
                len(files[("BOOT", "KERNEL64.ELF")]),
            ),
            _dir_entry(
                "ROOTFS.IMG",
                0x20,
                file_chains[("BOOT", "ROOTFS.IMG")][0],
                len(files[("BOOT", "ROOTFS.IMG")]),
            ),
            b"\x00" * 32,
        ],
    )

    for key, blob in files.items():
        chain = file_chains[key]
        for i, cluster in enumerate(chain):
            chunk = blob[i * cluster_size : (i + 1) * cluster_size]
            off = cluster_off(cluster)
            image[off : off + len(chunk)] = chunk

    out_path.write_bytes(image)
