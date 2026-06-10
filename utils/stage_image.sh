#!/bin/sh
set -eu

stage_dir=$1
boot_dir=$2
kernel_elf=$3
user_root=$4
mode=${5:-default}

check_stage_dir() {
    case "$1" in
        "" | "/" | "." | "..")
            echo "refusing unsafe stage directory: '$1'" >&2
            exit 1
            ;;
    esac
}

check_stage_dir "$stage_dir"
rm -rf "$stage_dir"
mkdir -p "$boot_dir"

cp -f "$kernel_elf" "$boot_dir/"
cp -r root/* "$stage_dir"
cp -a "$user_root"/. "$stage_dir"/
mkdir -p "$stage_dir/tmp"
chmod 1777 "$stage_dir/tmp"

if [ "$mode" = "riscv" ]; then
    rm -rf "$stage_dir/etc/cursors"
    rm -f "$stage_dir/home/user/wall.ppm"
fi
