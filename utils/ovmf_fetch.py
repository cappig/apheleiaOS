#!/usr/bin/env python3

from __future__ import annotations

import argparse
import io
import shutil
import tarfile
import time
import urllib.error
import urllib.request
from pathlib import Path


class FetchError(RuntimeError):
    pass


def _download_with_retries(url: str, out_path: Path, retries: int = 3, delay_sec: int = 2) -> None:
    last_error: Exception | None = None

    for attempt in range(1, retries + 1):
        try:
            with urllib.request.urlopen(url) as resp, out_path.open("wb") as out:
                shutil.copyfileobj(resp, out)
            return
        except Exception as exc:
            last_error = exc
            if attempt == retries:
                break
            time.sleep(delay_sec)

    raise FetchError(f"failed to download {url}: {last_error}")


def _read_ar_members(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    magic = b"!<arch>\n"
    if not data.startswith(magic):
        raise FetchError("unsupported OVMF package: invalid ar header")

    members: dict[str, bytes] = {}
    off = len(magic)

    while off + 60 <= len(data):
        hdr = data[off : off + 60]
        off += 60

        if hdr[58:60] != b"`\n":
            raise FetchError("unsupported OVMF package: invalid ar member header")

        name_raw = hdr[0:16].decode("ascii", errors="replace").strip()
        size_field = hdr[48:58].decode("ascii", errors="replace").strip()
        try:
            member_size = int(size_field)
        except ValueError as exc:
            raise FetchError("unsupported OVMF package: invalid ar member size") from exc

        if off + member_size > len(data):
            raise FetchError("unsupported OVMF package: truncated ar member")

        member_data = data[off : off + member_size]
        off += member_size
        if off % 2:
            off += 1

        if name_raw.startswith("#1/"):
            try:
                name_len = int(name_raw[3:])
            except ValueError as exc:
                raise FetchError("unsupported OVMF package: invalid extended ar name") from exc
            if name_len > len(member_data):
                raise FetchError("unsupported OVMF package: invalid extended ar payload")
            member_name = member_data[:name_len].decode("utf-8", errors="replace")
            payload = member_data[name_len:]
        else:
            member_name = name_raw.rstrip("/")
            payload = member_data

        members[member_name] = payload

    return members


def _decompress_data_tar(name: str, payload: bytes) -> bytes:
    if name == "data.tar":
        return payload
    if name == "data.tar.gz":
        import gzip

        return gzip.decompress(payload)
    if name == "data.tar.xz":
        import lzma

        return lzma.decompress(payload)
    if name == "data.tar.zst":
        try:
            import zstandard as zstd
        except ImportError as exc:
            raise FetchError("need Python package 'zstandard' to unpack data.tar.zst") from exc

        dctx = zstd.ZstdDecompressor()
        with dctx.stream_reader(io.BytesIO(payload)) as reader:
            return reader.read()

    raise FetchError(f"unsupported payload: {name}")


def _safe_extract_tar(data: bytes, dest: Path) -> None:
    base = dest.resolve()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as tf:
        for member in tf.getmembers():
            member_path = (dest / member.name).resolve()
            try:
                member_path.relative_to(base)
            except ValueError as exc:
                raise FetchError(f"unsafe path in tar payload: {member.name}") from exc
        tf.extractall(path=dest)


def main() -> None:
    parser = argparse.ArgumentParser(description="Download and extract OVMF files from a Debian package.")
    parser.add_argument("ovmf_dir", type=Path)
    parser.add_argument("ovmf_deb_url", type=str)
    args = parser.parse_args()

    ovmf_dir = args.ovmf_dir
    ovmf_deb = ovmf_dir / "ovmf.deb"
    ovmf_code = ovmf_dir / "OVMF_CODE.fd"
    ovmf_vars = ovmf_dir / "OVMF_VARS.fd"
    ovmf_stamp = ovmf_dir / ".extract.stamp"

    ovmf_dir.mkdir(parents=True, exist_ok=True)

    if ovmf_code.is_file() and ovmf_vars.is_file() and ovmf_stamp.is_file():
        return

    _download_with_retries(args.ovmf_deb_url, ovmf_deb)

    members = _read_ar_members(ovmf_deb)
    data_name = next((name for name in members if name.startswith("data.tar")), "")
    if not data_name:
        raise FetchError("unsupported OVMF package: missing data.tar payload")

    tar_data = _decompress_data_tar(data_name, members[data_name])
    _safe_extract_tar(tar_data, ovmf_dir)

    pkg_code = ovmf_dir / "usr/share/OVMF/OVMF_CODE.fd"
    pkg_vars = ovmf_dir / "usr/share/OVMF/OVMF_VARS.fd"

    if not pkg_code.is_file():
        raise FetchError("OVMF_CODE.fd not found in package payload")
    if not pkg_vars.is_file():
        raise FetchError("OVMF_VARS.fd not found in package payload")

    shutil.copyfile(pkg_code, ovmf_code)
    shutil.copyfile(pkg_vars, ovmf_vars)

    shutil.rmtree(ovmf_dir / "usr", ignore_errors=True)
    ovmf_stamp.write_text("ok\n", encoding="ascii")


if __name__ == "__main__":
    try:
        main()
    except (FetchError, urllib.error.URLError, tarfile.TarError, OSError) as e:
        raise SystemExit(f"error: {e}")
