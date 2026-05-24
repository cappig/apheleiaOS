#!/bin/sh
set -eu

stage_dir=$1
boot_dir=$2
kernel_elf=$3
user_root=$4
mode=${5:-default}

rm -rf "$stage_dir"
mkdir -p "$boot_dir"

cp -f "$kernel_elf" "$boot_dir/"
cp -r root/* "$stage_dir"
cp -a "$user_root"/. "$stage_dir"/

if [ "$mode" = "riscv" ]; then
    rm -rf "$stage_dir/usr/lib"
    rm -rf "$stage_dir/etc/cursors"
    rm -f "$stage_dir/home/user/wall.ppm"
fi
