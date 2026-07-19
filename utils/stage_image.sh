#!/bin/sh
set -eu

stage_dir=$1
boot_dir=$2
kernel_elf=$3
user_root=$4
strip_ui=${5:-false}
keep_sdk=${6:-true}
root_overlay=${7:-}
strip_sdk=${8:-false}

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

if [ -n "$root_overlay" ]; then
    cp -a "$root_overlay"/. "$stage_dir"/
fi

mkdir -p "$stage_dir/tmp"
chmod 1777 "$stage_dir/tmp"

if [ "$strip_ui" = "true" ]; then
    rm -rf "$stage_dir/etc/cursors"
    rm -f "$stage_dir/etc/wm.conf" "$stage_dir/home/user/wall.qoi"
fi

if [ "$strip_sdk" = "true" ] && [ "$keep_sdk" != "true" ]; then
    rm -rf "$stage_dir/usr/include" "$stage_dir/usr/lib"
    rmdir "$stage_dir/usr" 2>/dev/null || true
    rm -f "$stage_dir/home/user/cowsay.c" "$stage_dir/home/user/hello.c"
fi
