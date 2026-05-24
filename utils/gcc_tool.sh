#!/bin/sh
set -eu

cc=$1
prog=$2
shift 2

if [ -n "$cc" ]; then
    path=$($cc -print-prog-name="$prog" 2>/dev/null || true)

    if [ -n "$path" ] && [ "$path" != "$prog" ]; then
        if [ -x "$path" ] || command -v "$path" >/dev/null 2>&1; then
            printf '%s\n' "$path"
            exit 0
        fi
    fi
fi

for tool in "$@"; do
    if [ -n "$tool" ] && command -v "$tool" >/dev/null 2>&1; then
        printf '%s\n' "$tool"
        exit 0
    fi
done

exit 0
