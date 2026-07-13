#!/bin/sh
set -eu

image=$1
platform=$2
repo=$3
make_cmd=$4
arch=${5:-}
toolchain=${6:-}
image_format=${7:-}
profile=${8:-}
riscv_frisc=${9:-}
riscv_uart_stride=${10:-}
userland=${11:-}
source_date_epoch=${12:-}
build_date=${13:-}
git_commit_short=${14:-}
traceable_kernel=${15:-}
boot_log_color=${16:-}
strip_kernel=${17:-}
strip_user=${18:-}
strip_user_symbols=${19:-}
rootfs_extra_bytes=${20:-}
strip_kernel_flags=${21:-}
user_strip_flags=${22:-}
gcc_analyzer=${23:-}
riscv_uart0=${24:-}
riscv_scratch_offset=${25:-}

set --

if [ -n "$arch" ]; then
    set -- "$@" ARCH="$arch"
fi

if [ -n "$toolchain" ]; then
    set -- "$@" TOOLCHAIN="$toolchain"
fi

if [ -n "$image_format" ]; then
    set -- "$@" IMAGE_FORMAT="$image_format"
fi

if [ -n "$profile" ]; then
    set -- "$@" PROFILE="$profile"
fi

if [ -n "$build_date" ]; then
    set -- "$@" BUILD_DATE="$build_date"
fi

if [ -n "$git_commit_short" ]; then
    set -- "$@" GIT_COMMIT_SHORT="$git_commit_short"
fi

if [ -n "$traceable_kernel" ]; then
    set -- "$@" TRACEABLE_KERNEL="$traceable_kernel"
fi

if [ -n "$boot_log_color" ]; then
    set -- "$@" BOOT_LOG_COLOR="$boot_log_color"
fi

if [ -n "$riscv_frisc" ]; then
    set -- "$@" RISCV_FRISC="$riscv_frisc"
fi

if [ -n "$riscv_uart_stride" ]; then
    set -- "$@" RISCV_UART_STRIDE="$riscv_uart_stride"
fi

if [ -n "$userland" ]; then
    set -- "$@" USERLAND="$userland"
fi

if [ -n "$source_date_epoch" ]; then
    set -- "$@" SOURCE_DATE_EPOCH="$source_date_epoch"
fi

if [ -n "$strip_kernel" ]; then
    set -- "$@" STRIP_KERNEL="$strip_kernel"
fi

if [ -n "$strip_user" ]; then
    set -- "$@" STRIP_USER="$strip_user"
fi

if [ -n "$strip_user_symbols" ]; then
    set -- "$@" STRIP_USER_SYMBOLS="$strip_user_symbols"
fi

if [ -n "$rootfs_extra_bytes" ]; then
    set -- "$@" ROOTFS_EXTRA_BYTES="$rootfs_extra_bytes"
fi

if [ -n "$strip_kernel_flags" ]; then
    set -- "$@" STRIP_KERNEL_FLAGS="$strip_kernel_flags"
fi

if [ -n "$user_strip_flags" ]; then
    set -- "$@" USER_STRIP_FLAGS="$user_strip_flags"
fi

if [ -n "$gcc_analyzer" ]; then
    set -- "$@" GCC_ANALYZER="$gcc_analyzer"
fi

if [ -n "$riscv_uart0" ]; then
    set -- "$@" RISCV_UART0="$riscv_uart0"
fi

if [ -n "$riscv_scratch_offset" ]; then
    set -- "$@" RISCV_SCRATCH_OFFSET="$riscv_scratch_offset"
fi

if uid=$(id -u 2>/dev/null) && gid=$(id -g 2>/dev/null); then
    docker run --rm \
        --platform "$platform" \
        --user "$uid:$gid" \
        -v "$repo":/usr/src/apheleia \
        -w /usr/src/apheleia \
        "$image" "$make_cmd" "$@"
else
    docker run --rm \
        --platform "$platform" \
        -v "$repo":/usr/src/apheleia \
        -w /usr/src/apheleia \
        "$image" "$make_cmd" "$@"
fi
