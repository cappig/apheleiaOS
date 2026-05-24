#!/bin/sh
set -eu

stamp=$1
text=$2
tmp="$stamp.tmp"

mkdir -p "$(dirname "$stamp")"
printf '%s\n' "$text" > "$tmp"

if cmp -s "$tmp" "$stamp"; then
    rm -f "$tmp"
else
    mv "$tmp" "$stamp"
fi
