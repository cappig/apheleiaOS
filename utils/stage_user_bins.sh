#!/bin/sh
set -eu

stage_dir=$1
bin_dir=$2
shift 2

check_stage_dir() {
    case "$1" in
        "" | "/" | "." | "..")
            echo "refusing unsafe stage directory: '$1'" >&2
            exit 1
            ;;
    esac
}

check_stage_dir "$stage_dir"
mkdir -p "$stage_dir"
find "$stage_dir" -mindepth 1 -maxdepth 1 ! -name '.apheleia_*' -exec rm -rf -- {} +

for bin in "$@"; do
    cp "$bin_dir/$bin" "$stage_dir/$bin"
done
