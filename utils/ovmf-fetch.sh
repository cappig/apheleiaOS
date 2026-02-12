#!/usr/bin/env sh

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <ovmf_dir> <ovmf_deb_url>" >&2
    exit 1
fi

ovmf_dir="$1"
ovmf_deb_url="$2"
ovmf_deb="$ovmf_dir/ovmf.deb"
ovmf_code="$ovmf_dir/OVMF_CODE.fd"
ovmf_vars="$ovmf_dir/OVMF_VARS.fd"
ovmf_stamp="$ovmf_dir/.extract.stamp"

mkdir -p "$ovmf_dir"

if [ -f "$ovmf_code" ] && [ -f "$ovmf_vars" ] && [ -f "$ovmf_stamp" ]; then
    exit 0
fi

if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --retry-delay 2 -o "$ovmf_deb" "$ovmf_deb_url"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$ovmf_deb" "$ovmf_deb_url"
else
    echo "missing downloader (need curl or wget)" >&2
    exit 1
fi

data_tar="$(ar t "$ovmf_deb" | awk '/^data\.tar/{print; exit}')"
if [ -z "$data_tar" ]; then
    echo "unsupported OVMF package: missing data.tar payload" >&2
    exit 1
fi

case "$data_tar" in
data.tar.xz)
    ar p "$ovmf_deb" "$data_tar" | tar -xJ -C "$ovmf_dir"
    ;;
data.tar.gz)
    ar p "$ovmf_deb" "$data_tar" | tar -xz -C "$ovmf_dir"
    ;;
data.tar.zst)
    if command -v zstd >/dev/null 2>&1; then
        ar p "$ovmf_deb" "$data_tar" | zstd -dc | tar -x -C "$ovmf_dir"
    else
        echo "need zstd to unpack $data_tar" >&2
        exit 1
    fi
    ;;
data.tar)
    ar p "$ovmf_deb" "$data_tar" | tar -x -C "$ovmf_dir"
    ;;
*)
    echo "unsupported payload: $data_tar" >&2
    exit 1
    ;;
esac

pkg_code="$ovmf_dir/usr/share/OVMF/OVMF_CODE.fd"
pkg_vars="$ovmf_dir/usr/share/OVMF/OVMF_VARS.fd"

if [ ! -f "$pkg_code" ]; then
    echo "OVMF_CODE.fd not found in package payload" >&2
    exit 1
fi

if [ ! -f "$pkg_vars" ]; then
    echo "OVMF_VARS.fd not found in package payload" >&2
    exit 1
fi

cp -f "$pkg_code" "$ovmf_code"
cp -f "$pkg_vars" "$ovmf_vars"

rm -rf "$ovmf_dir/usr"
touch "$ovmf_stamp"
