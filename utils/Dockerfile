FROM alpine:3.24.1

WORKDIR /usr/src/apheleia

RUN apk add --no-cache \
    binutils~2 \
    binutils-riscv-none-elf~2 \
    clang22~22 \
    curl~8 \
    dtc~1 \
    gcc~15 \
    gcc-riscv-none-elf~16 \
    git~2 \
    lld22~22 \
    llvm22~22 \
    make~4 \
    mingw-w64-gcc~15 \
    mingw-w64-binutils~2 \
    nasm~3 \
    python3~3 \
    xorriso~1 \
    && ln -sf clang-22 /usr/bin/clang \
    && ln -sf llvm22-ar /usr/bin/llvm-ar \
    && ln -sf llvm22-objcopy /usr/bin/llvm-objcopy \
    && ln -sf llvm22-strip /usr/bin/llvm-strip
