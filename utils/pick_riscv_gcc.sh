#!/bin/sh
set -eu

march=$1
mabi=$2
shift 2

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cat > "$tmp/probe.c" <<'EOF'
double __apheleia_probe(double a, double b) {
    return a / b;
}
EOF

for cc in "$@"; do
    if [ -z "$cc" ] || ! command -v "$cc" >/dev/null 2>&1; then
        continue
    fi

    libgcc=$($cc -march="$march" -mabi="$mabi" -print-libgcc-file-name 2>/dev/null || true)
    if [ ! -f "$libgcc" ]; then
        continue
    fi

    if ! $cc -march="$march" -mabi="$mabi" -ffreestanding \
        -c "$tmp/probe.c" -o "$tmp/probe.o" >/dev/null 2>&1; then
        continue
    fi

    if $cc -march="$march" -mabi="$mabi" -nostdlib -r \
        "$tmp/probe.o" "$libgcc" -o "$tmp/probe-link.o" >/dev/null 2>&1; then
        printf '%s\n' "$cc"
        exit 0
    fi
done

exit 0
