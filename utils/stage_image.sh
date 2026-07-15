#!/bin/sh
set -eu

stage_dir=$1
boot_dir=$2
kernel_elf=$3
user_root=$4
mode=${5:-default}
keep_sdk=${6:-true}

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
cp -a root/. "$stage_dir"/
cp -a "$user_root"/. "$stage_dir"/
mkdir -p "$stage_dir/tmp"
chmod 1777 "$stage_dir/tmp"

if [ "$mode" = "riscv" ] || [ "$mode" = "frisc" ]; then
    # ttyS0 is an alias of tty0 on RISC-V, so it must not run a second getty.
    sed -i '\|^/dev/ttyS0[[:space:]]|d' "$stage_dir/etc/ttys"
    rm -rf "$stage_dir/etc/cursors"
    rm -f "$stage_dir/etc/wm.conf" "$stage_dir/home/user/wall.qoi"
fi

if [ "$mode" = "frisc" ] && [ "$keep_sdk" != "true" ]; then
    rm -rf "$stage_dir/usr/include" "$stage_dir/usr/lib"
    rmdir "$stage_dir/usr" 2>/dev/null || true
    rm -f "$stage_dir/home/user/cowsay.c" "$stage_dir/home/user/hello.c"
fi
