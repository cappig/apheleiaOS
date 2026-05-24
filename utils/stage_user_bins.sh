#!/bin/sh
set -eu

stage_dir=$1
bin_dir=$2
shift 2

mkdir -p "$stage_dir"
find "$stage_dir" -mindepth 1 -maxdepth 1 ! -name '.apheleia_*' -exec rm -rf -- {} +

for bin in "$@"; do
    cp "$bin_dir/$bin" "$stage_dir/$bin"
done
