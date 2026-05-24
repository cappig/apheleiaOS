#!/bin/sh
set -eu

for tool in "$@"; do
    if [ -n "$tool" ] && command -v "$tool" >/dev/null 2>&1; then
        printf '%s\n' "$tool"
        exit 0
    fi
done

exit 0
