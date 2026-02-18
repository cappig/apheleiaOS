#!/usr/bin/env python3

from __future__ import annotations

import argparse
import struct
import sys
import tempfile
import time
from pathlib import Path

from build_image_common import (
    BuildError,
    SECTOR_SIZE,
    build_esp_fat16_image,
    build_ext2_image,
    div_round_up,
    prepare_root_tree,
    write_at,
    write_file_to_lba,
    write_gpt,
    write_mbr,
)

ISO_SECTOR_SIZE = 2048
VD_PRIMARY_LBA = 16
VD_BOOT_RECORD_LBA = 17
VD_TERMINATOR_LBA = 18

GPT_ESP_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
GPT_LINUX_GUID = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"


class IsoLayout:
    def __init__(
        self,
        *,
        l_path_lba: int,
        m_path_lba: int,
        path_table_size: int,
        root_dir_lba: int,
        root_dir_size: int,
        boot_dir_lba: int,
        boot_dir_size: int,
        boot_catalog_lba: int,
        bios_lba: int,
        bios_size: int,
        rootfs_lba: int,
        rootfs_size: int,
        efi_lba: int | None,
        efi_size: int,
        iso_blocks: int,
    ) -> None:
        self.l_path_lba = l_path_lba
        self.m_path_lba = m_path_lba
        self.path_table_size = path_table_size
        self.root_dir_lba = root_dir_lba
        self.root_dir_size = root_dir_size
        self.boot_dir_lba = boot_dir_lba
        self.boot_dir_size = boot_dir_size
        self.boot_catalog_lba = boot_catalog_lba
        self.bios_lba = bios_lba
        self.bios_size = bios_size
        self.rootfs_lba = rootfs_lba
        self.rootfs_size = rootfs_size
        self.efi_lba = efi_lba
        self.efi_size = efi_size
        self.iso_blocks = iso_blocks


def _pad_ascii(value: str, length: int) -> bytes:
    raw = value.encode("ascii", errors="replace")[:length]
    return raw.ljust(length, b" ")


def _u16_both(value: int) -> bytes:
    return struct.pack("<H", value) + struct.pack(">H", value)


def _u32_both(value: int) -> bytes:
    return struct.pack("<I", value) + struct.pack(">I", value)


def _rec_time_7(epoch: int) -> bytes:
    tm = time.gmtime(epoch)
    year = max(0, min(255, tm.tm_year - 1900))
    return bytes(
        [
            year,
            tm.tm_mon,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            0,
        ]
    )


def _vol_time_17(epoch: int) -> bytes:
    tm = time.gmtime(epoch)
    text = time.strftime("%Y%m%d%H%M%S", tm)
    return (text + "00").encode("ascii") + b"\x00"


def _iso_dir_record(
    *,
    name: bytes,
    extent_lba: int,
    data_size: int,
    is_dir: bool,
    timestamp: int,
) -> bytes:
    if len(name) > 255:
        raise BuildError("ISO directory identifier too long")

    pad = 1 if (len(name) % 2 == 0) else 0
    length = 33 + len(name) + pad
    rec = bytearray(length)

    rec[0] = length
    rec[1] = 0
    rec[2:10] = _u32_both(extent_lba)
    rec[10:18] = _u32_both(data_size)
    rec[18:25] = _rec_time_7(timestamp)
    rec[25] = 0x02 if is_dir else 0x00
    rec[26] = 0
    rec[27] = 0
    rec[28:32] = _u16_both(1)
    rec[32] = len(name)
    rec[33 : 33 + len(name)] = name
    return bytes(rec)


def _pack_directory(records: list[bytes]) -> bytes:
    out = bytearray()
    used = 0

    for rec in records:
        rec_len = len(rec)
        if rec_len > ISO_SECTOR_SIZE:
            raise BuildError("ISO directory record too large")

        if used + rec_len > ISO_SECTOR_SIZE:
            out += b"\x00" * (ISO_SECTOR_SIZE - used)
            used = 0

        out += rec
        used += rec_len

        if used == ISO_SECTOR_SIZE:
            used = 0

    if used:
        out += b"\x00" * (ISO_SECTOR_SIZE - used)

    if not out:
        out = bytearray(ISO_SECTOR_SIZE)

    return bytes(out)


def _path_table_entry(*, name: bytes, extent_lba: int, parent: int, big_endian: bool) -> bytes:
    if len(name) > 255:
        raise BuildError("ISO path table identifier too long")

    out = bytearray()
    out.append(len(name))
    out.append(0)

    if big_endian:
        out += struct.pack(">I", extent_lba)
        out += struct.pack(">H", parent)
    else:
        out += struct.pack("<I", extent_lba)
        out += struct.pack("<H", parent)

    out += name
    if len(name) % 2:
        out.append(0)

    return bytes(out)


def _build_path_tables(root_lba: int, boot_lba: int) -> tuple[bytes, bytes, int]:
    l_entries = [
        _path_table_entry(name=b"\x00", extent_lba=root_lba, parent=1, big_endian=False),
        _path_table_entry(name=b"BOOT", extent_lba=boot_lba, parent=1, big_endian=False),
    ]
    m_entries = [
        _path_table_entry(name=b"\x00", extent_lba=root_lba, parent=1, big_endian=True),
        _path_table_entry(name=b"BOOT", extent_lba=boot_lba, parent=1, big_endian=True),
    ]

    l_raw = b"".join(l_entries)
    m_raw = b"".join(m_entries)
    if len(l_raw) != len(m_raw):
        raise BuildError("ISO path table size mismatch")

    return l_raw, m_raw, len(l_raw)


def _build_boot_catalog(*, bios_lba: int, bios_load_sectors_512: int, efi_lba: int | None) -> bytes:
    if bios_load_sectors_512 > 0xFFFF:
        raise BuildError("bios.bin is too large for El Torito load-size field")

    catalog = bytearray(ISO_SECTOR_SIZE)

    validation = bytearray(32)
    validation[0] = 0x01
    validation[1] = 0x00
    validation[4:28] = _pad_ascii("APHELEIA BOOT", 24)
    validation[30] = 0x55
    validation[31] = 0xAA

    words = struct.unpack("<16H", bytes(validation))
    checksum = (-sum(words)) & 0xFFFF
    struct.pack_into("<H", validation, 28, checksum)

    default_entry = bytearray(32)
    default_entry[0] = 0x88
    default_entry[1] = 0x00
    struct.pack_into("<H", default_entry, 6, bios_load_sectors_512)
    struct.pack_into("<I", default_entry, 8, bios_lba)

    catalog[0:32] = validation
    catalog[32:64] = default_entry

    if efi_lba is not None:
        section_header = bytearray(32)
        section_header[0] = 0x91
        section_header[1] = 0xEF
        struct.pack_into("<H", section_header, 2, 1)
        section_header[4:32] = _pad_ascii("UEFI", 28)

        section_entry = bytearray(32)
        section_entry[0] = 0x88
        section_entry[1] = 0x00
        section_entry[4] = 0xEF
        struct.pack_into("<H", section_entry, 6, 0)
        struct.pack_into("<I", section_entry, 8, efi_lba)

        catalog[64:96] = section_header
        catalog[96:128] = section_entry

    return bytes(catalog)


def _build_pvd(
    *,
    volume_id: str,
    total_blocks: int,
    path_table_size: int,
    l_path_lba: int,
    m_path_lba: int,
    root_record: bytes,
    now: int,
) -> bytes:
    pvd = bytearray(ISO_SECTOR_SIZE)
    pvd[0] = 0x01
    pvd[1:6] = b"CD001"
    pvd[6] = 0x01

    pvd[8:40] = _pad_ascii("APHELEIA", 32)
    pvd[40:72] = _pad_ascii(volume_id, 32)
    pvd[80:88] = _u32_both(total_blocks)
    pvd[120:124] = _u16_both(1)
    pvd[124:128] = _u16_both(1)
    pvd[128:132] = _u16_both(ISO_SECTOR_SIZE)
    pvd[132:140] = _u32_both(path_table_size)

    struct.pack_into("<I", pvd, 140, l_path_lba)
    struct.pack_into("<I", pvd, 144, 0)
    struct.pack_into(">I", pvd, 148, m_path_lba)
    struct.pack_into(">I", pvd, 152, 0)

    if len(root_record) != 34:
        raise BuildError("invalid root directory record size")
    pvd[156:190] = root_record

    pvd[190:318] = _pad_ascii("APHELEIA", 128)
    pvd[318:446] = _pad_ascii("APHELEIA", 128)
    pvd[446:574] = _pad_ascii("APHELEIA", 128)
    pvd[574:702] = _pad_ascii("APHELEIA", 128)
    pvd[702:739] = _pad_ascii("apheleiaOS", 37)

    ts = _vol_time_17(now)
    pvd[813:830] = ts
    pvd[830:847] = ts
    pvd[847:864] = b"0" * 16 + b"\x00"
    pvd[864:881] = b"0" * 16 + b"\x00"

    pvd[881] = 0x01
    return bytes(pvd)


def _build_boot_record(*, boot_catalog_lba: int) -> bytes:
    br = bytearray(ISO_SECTOR_SIZE)
    br[0] = 0x00
    br[1:6] = b"CD001"
    br[6] = 0x01
    system_id = b"EL TORITO SPECIFICATION"
    br[7 : 7 + len(system_id)] = system_id
    struct.pack_into("<I", br, 71, boot_catalog_lba)
    return bytes(br)


def _build_terminator() -> bytes:
    term = bytearray(ISO_SECTOR_SIZE)
    term[0] = 0xFF
    term[1:6] = b"CD001"
    term[6] = 0x01
    return bytes(term)


def _layout_iso(
    *,
    bios_size: int,
    rootfs_size: int,
    efi_size: int,
) -> IsoLayout:
    # Minimal ISO has two directories (root + BOOT), one boot catalog, and file extents.
    path_table_size = len(
        _path_table_entry(name=b"\x00", extent_lba=0, parent=1, big_endian=False)
    ) + len(_path_table_entry(name=b"BOOT", extent_lba=0, parent=1, big_endian=False))

    path_table_sectors = div_round_up(path_table_size, ISO_SECTOR_SIZE)
    root_dir_size = ISO_SECTOR_SIZE
    boot_dir_size = ISO_SECTOR_SIZE
    boot_catalog_size = ISO_SECTOR_SIZE

    l_path_lba = VD_TERMINATOR_LBA + 1
    m_path_lba = l_path_lba + path_table_sectors
    root_dir_lba = m_path_lba + path_table_sectors
    boot_dir_lba = root_dir_lba + div_round_up(root_dir_size, ISO_SECTOR_SIZE)
    boot_catalog_lba = boot_dir_lba + div_round_up(boot_dir_size, ISO_SECTOR_SIZE)

    bios_lba = boot_catalog_lba + div_round_up(boot_catalog_size, ISO_SECTOR_SIZE)
    bios_blocks = div_round_up(bios_size, ISO_SECTOR_SIZE)

    rootfs_lba = bios_lba + bios_blocks
    rootfs_blocks = div_round_up(rootfs_size, ISO_SECTOR_SIZE)

    efi_lba: int | None = None
    efi_blocks = 0
    if efi_size:
        efi_lba = rootfs_lba + rootfs_blocks
        efi_blocks = div_round_up(efi_size, ISO_SECTOR_SIZE)

    iso_blocks = rootfs_lba + rootfs_blocks + efi_blocks

    return IsoLayout(
        l_path_lba=l_path_lba,
        m_path_lba=m_path_lba,
        path_table_size=path_table_size,
        root_dir_lba=root_dir_lba,
        root_dir_size=root_dir_size,
        boot_dir_lba=boot_dir_lba,
        boot_dir_size=boot_dir_size,
        boot_catalog_lba=boot_catalog_lba,
        bios_lba=bios_lba,
        bios_size=bios_size,
        rootfs_lba=rootfs_lba,
        rootfs_size=rootfs_size,
        efi_lba=efi_lba,
        efi_size=efi_size,
        iso_blocks=iso_blocks,
    )


def _build_directories(layout: IsoLayout, now: int) -> tuple[bytes, bytes, bytes]:
    root_record = _iso_dir_record(
        name=b"\x00",
        extent_lba=layout.root_dir_lba,
        data_size=layout.root_dir_size,
        is_dir=True,
        timestamp=now,
    )
    parent_record = _iso_dir_record(
        name=b"\x01",
        extent_lba=layout.root_dir_lba,
        data_size=layout.root_dir_size,
        is_dir=True,
        timestamp=now,
    )
    boot_dir_record = _iso_dir_record(
        name=b"BOOT",
        extent_lba=layout.boot_dir_lba,
        data_size=layout.boot_dir_size,
        is_dir=True,
        timestamp=now,
    )

    root_dir = _pack_directory([root_record, parent_record, boot_dir_record])

    boot_self = _iso_dir_record(
        name=b"\x00",
        extent_lba=layout.boot_dir_lba,
        data_size=layout.boot_dir_size,
        is_dir=True,
        timestamp=now,
    )
    boot_parent = _iso_dir_record(
        name=b"\x01",
        extent_lba=layout.root_dir_lba,
        data_size=layout.root_dir_size,
        is_dir=True,
        timestamp=now,
    )
    boot_cat = _iso_dir_record(
        name=b"BOOT.CAT;1",
        extent_lba=layout.boot_catalog_lba,
        data_size=ISO_SECTOR_SIZE,
        is_dir=False,
        timestamp=now,
    )
    bios_rec = _iso_dir_record(
        name=b"BIOS.BIN;1",
        extent_lba=layout.bios_lba,
        data_size=layout.bios_size,
        is_dir=False,
        timestamp=now,
    )
    rootfs_rec = _iso_dir_record(
        name=b"ROOTFS.IMG;1",
        extent_lba=layout.rootfs_lba,
        data_size=layout.rootfs_size,
        is_dir=False,
        timestamp=now,
    )

    boot_records = [boot_self, boot_parent, boot_cat, bios_rec, rootfs_rec]

    if layout.efi_lba is not None and layout.efi_size:
        efi_rec = _iso_dir_record(
            name=b"EFI.IMG;1",
            extent_lba=layout.efi_lba,
            data_size=layout.efi_size,
            is_dir=False,
            timestamp=now,
        )
        boot_records.append(efi_rec)

    boot_dir = _pack_directory(boot_records)
    return root_record, root_dir, boot_dir


def _write_iso(
    *,
    output_iso: Path,
    mbr_bin: Path,
    bios_bin: Path,
    rootfs_img: Path,
    efi_img: Path | None,
) -> None:
    bios_size = bios_bin.stat().st_size
    rootfs_size = rootfs_img.stat().st_size
    efi_size = efi_img.stat().st_size if efi_img else 0

    layout = _layout_iso(bios_size=bios_size, rootfs_size=rootfs_size, efi_size=efi_size)

    # Keep ISO file 2048-byte aligned while reserving space for GPT backup
    # structures. 36 * 512 = 9 * 2048 bytes.
    gpt_tail_sectors = 36 if efi_img else 0
    total_512_sectors = layout.iso_blocks * (ISO_SECTOR_SIZE // SECTOR_SIZE) + gpt_tail_sectors

    with output_iso.open("wb") as f:
        f.truncate(total_512_sectors * SECTOR_SIZE)

    now = int(time.time())
    root_record, root_dir_blob, boot_dir_blob = _build_directories(layout, now)

    l_path_raw, m_path_raw, path_table_size = _build_path_tables(layout.root_dir_lba, layout.boot_dir_lba)
    if path_table_size != layout.path_table_size:
        raise BuildError("internal ISO path table sizing mismatch")

    boot_catalog = _build_boot_catalog(
        bios_lba=layout.bios_lba,
        bios_load_sectors_512=div_round_up(bios_size, SECTOR_SIZE),
        efi_lba=layout.efi_lba,
    )

    pvd = _build_pvd(
        volume_id="APHELEIAOS",
        total_blocks=layout.iso_blocks,
        path_table_size=layout.path_table_size,
        l_path_lba=layout.l_path_lba,
        m_path_lba=layout.m_path_lba,
        root_record=root_record,
        now=now,
    )
    boot_record = _build_boot_record(boot_catalog_lba=layout.boot_catalog_lba)
    term = _build_terminator()

    write_at(output_iso, VD_PRIMARY_LBA * ISO_SECTOR_SIZE, pvd)
    write_at(output_iso, VD_BOOT_RECORD_LBA * ISO_SECTOR_SIZE, boot_record)
    write_at(output_iso, VD_TERMINATOR_LBA * ISO_SECTOR_SIZE, term)

    write_at(output_iso, layout.l_path_lba * ISO_SECTOR_SIZE, l_path_raw)
    write_at(output_iso, layout.m_path_lba * ISO_SECTOR_SIZE, m_path_raw)
    write_at(output_iso, layout.root_dir_lba * ISO_SECTOR_SIZE, root_dir_blob)
    write_at(output_iso, layout.boot_dir_lba * ISO_SECTOR_SIZE, boot_dir_blob)
    write_at(output_iso, layout.boot_catalog_lba * ISO_SECTOR_SIZE, boot_catalog)

    write_file_to_lba(output_iso, bios_bin, layout.bios_lba, sector_size=ISO_SECTOR_SIZE)
    write_file_to_lba(output_iso, rootfs_img, layout.rootfs_lba, sector_size=ISO_SECTOR_SIZE)
    if efi_img and layout.efi_lba is not None:
        write_file_to_lba(output_iso, efi_img, layout.efi_lba, sector_size=ISO_SECTOR_SIZE)

    bios_start_512 = layout.bios_lba * (ISO_SECTOR_SIZE // SECTOR_SIZE)
    bios_sectors_512 = div_round_up(bios_size, SECTOR_SIZE)

    rootfs_start_512 = layout.rootfs_lba * (ISO_SECTOR_SIZE // SECTOR_SIZE)
    rootfs_sectors_512 = div_round_up(rootfs_size, SECTOR_SIZE)

    mbr_entries = [
        (0x80, 0x83, bios_start_512, bios_sectors_512),
        (0x00, 0x83, rootfs_start_512, rootfs_sectors_512),
    ]

    if efi_img and layout.efi_lba is not None:
        efi_start_512 = layout.efi_lba * (ISO_SECTOR_SIZE // SECTOR_SIZE)
        efi_sectors_512 = div_round_up(efi_size, SECTOR_SIZE)
        mbr_entries.append((0x00, 0xEF, efi_start_512, efi_sectors_512))
        mbr_entries.append((0x00, 0xEE, 1, min(total_512_sectors - 1, 0xFFFFFFFF)))

        write_gpt(
            output_iso,
            total_sectors=total_512_sectors,
            partitions=[
                {
                    "type_guid": GPT_LINUX_GUID,
                    "name": "apheleiaOS",
                    "start_lba": rootfs_start_512,
                    "end_lba": rootfs_start_512 + rootfs_sectors_512 - 1,
                },
                {
                    "type_guid": GPT_ESP_GUID,
                    "name": "EFI System",
                    "start_lba": efi_start_512,
                    "end_lba": efi_start_512 + efi_sectors_512 - 1,
                },
            ],
        )

    write_mbr(
        output_iso,
        code440=mbr_bin.read_bytes()[:440],
        entries=mbr_entries,
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Create BIOS/UEFI bootable ISO using pure Python image builders."
    )
    parser.add_argument("output_iso", type=Path)
    parser.add_argument("mbr_bin", type=Path)
    parser.add_argument("bios_bin", type=Path)
    parser.add_argument("efi_app", nargs="?", default="", type=str)
    parser.add_argument("kernel_elf", nargs="?", default="", type=str)
    parser.add_argument("rootfs_dir", type=Path)
    args = parser.parse_args()

    efi_app = Path(args.efi_app) if args.efi_app else None
    kernel_elf = Path(args.kernel_elf) if args.kernel_elf else None
    uefi_enabled = bool(efi_app and kernel_elf and efi_app.is_file() and kernel_elf.is_file())

    with tempfile.TemporaryDirectory(prefix="apheleia-iso-") as td:
        td_path = Path(td)
        root_tree = td_path / "root"
        ext2_img = td_path / "rootfs.ext2"
        esp_img = td_path / "esp.img"

        prepare_root_tree(args.rootfs_dir, root_tree)
        build_ext2_image(
            root_tree,
            ext2_img,
            block_size=4096,
            inode_count=2048,
            growth_numerator=2,
            growth_denominator=1,
        )

        efi_img: Path | None = None
        if uefi_enabled:
            build_esp_fat16_image(
                esp_img,
                size_sectors=131072,  # 64 MiB
                efi_app=efi_app,
                kernel_elf=kernel_elf,
                rootfs_image=ext2_img,
            )
            efi_img = esp_img

        _write_iso(
            output_iso=args.output_iso,
            mbr_bin=args.mbr_bin,
            bios_bin=args.bios_bin,
            rootfs_img=ext2_img,
            efi_img=efi_img,
        )

    print(f"ISO {args.output_iso}")


if __name__ == "__main__":
    try:
        main()
    except (BuildError, RuntimeError) as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
