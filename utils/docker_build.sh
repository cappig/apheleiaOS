#!/bin/sh
set -eu

image=$1
repo=$2
make_cmd=$3
arch=$4
toolchain=$5
image_format=$6
profile=$7
riscv_frisc=${8:-}
riscv_uart_stride=${9:-}

set -- \
    ARCH="$arch" \
    TOOLCHAIN="$toolchain" \
    IMAGE_FORMAT="$image_format" \
    PROFILE="$profile"

if [ -n "$riscv_frisc" ]; then
    set -- "$@" RISCV_FRISC="$riscv_frisc"
fi

if [ -n "$riscv_uart_stride" ]; then
    set -- "$@" RISCV_UART_STRIDE="$riscv_uart_stride"
fi

set -- "$@" \
    GNU_CC_x86_64=gcc \
    GNU_AR_x86_64=ar \
    GNU_CC_x86_32=gcc \
    GNU_AR_x86_32=ar \
    GNU_LD_x86_32=ld \
    GNU_OC_x86_32=objcopy \
    GNU_ST_x86_32=strip \
    GNU_CC_riscv_64=riscv-none-elf-gcc \
    GNU_AR_riscv_64=riscv-none-elf-ar \
    GNU_LD_riscv_64=riscv-none-elf-ld \
    GNU_OC_riscv_64=riscv-none-elf-objcopy \
    GNU_ST_riscv_64=riscv-none-elf-strip \
    GNU_CC_riscv_32=riscv-none-elf-gcc \
    GNU_AR_riscv_32=riscv-none-elf-ar \
    GNU_LD_riscv_32=riscv-none-elf-ld \
    GNU_OC_riscv_32=riscv-none-elf-objcopy \
    GNU_ST_riscv_32=riscv-none-elf-strip \
    LLVM_AR_x86_64=llvm18-ar \
    LLVM_AR_x86_32=llvm18-ar \
    LLVM_AR_riscv_64=llvm18-ar \
    LLVM_AR_riscv_32=llvm18-ar \
    LLVM_CC_x86_64=clang-18 \
    LLVM_CC_x86_32=clang-18 \
    LLVM_CC_riscv_64="clang-18 --target=riscv64-unknown-elf" \
    LLVM_CC_riscv_32="clang-18 --target=riscv32-unknown-elf" \
    LLVM_OC_x86_64=llvm18-objcopy \
    LLVM_OC_x86_32=llvm18-objcopy \
    LLVM_OC_riscv_64=llvm18-objcopy \
    LLVM_OC_riscv_32=llvm18-objcopy \
    LLVM_ST_x86_64=llvm18-strip \
    LLVM_ST_x86_32=llvm18-strip \
    LLVM_ST_riscv_64=llvm18-strip \
    LLVM_ST_riscv_32=llvm18-strip

if uid=$(id -u 2>/dev/null) && gid=$(id -g 2>/dev/null); then
    docker run --rm \
        --user "$uid:$gid" \
        -v "$repo":/usr/src/apheleia \
        -w /usr/src/apheleia \
        "$image" "$make_cmd" "$@"
else
    docker run --rm \
        -v "$repo":/usr/src/apheleia \
        -w /usr/src/apheleia \
        "$image" "$make_cmd" "$@"
fi
