#!/bin/sh
set -eu

image=$1
repo=$2
make_cmd=$3
arch=$4
toolchain=$5
image_format=$6
profile=$7

user_args=
if uid=$(id -u 2>/dev/null) && gid=$(id -g 2>/dev/null); then
    user_args="--user $uid:$gid"
fi

docker run --rm $user_args \
    -v "$repo":/usr/src/apheleia \
    -w /usr/src/apheleia \
    "$image" "$make_cmd" \
    ARCH="$arch" \
    TOOLCHAIN="$toolchain" \
    IMAGE_FORMAT="$image_format" \
    PROFILE="$profile" \
    GNU_CC_x86_64=gcc \
    GNU_CC_x86_32=gcc \
    GNU_LD_x86_32=ld \
    GNU_OC_x86_32=objcopy \
    GNU_ST_x86_32=strip \
    GNU_CC_riscv_64=riscv-none-elf-gcc \
    GNU_LD_riscv_64=riscv-none-elf-ld \
    GNU_OC_riscv_64=riscv-none-elf-objcopy \
    GNU_ST_riscv_64=riscv-none-elf-strip \
    GNU_CC_riscv_32=riscv-none-elf-gcc \
    GNU_LD_riscv_32=riscv-none-elf-ld \
    GNU_OC_riscv_32=riscv-none-elf-objcopy \
    GNU_ST_riscv_32=riscv-none-elf-strip \
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
