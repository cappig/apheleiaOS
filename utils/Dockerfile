FROM alpine:3.20

WORKDIR /usr/src/apheleia

RUN apk add --no-cache \
    binutils~2 \
    binutils-riscv-none-elf \
    clang18 \
    curl~8 \
    gcc~13 \
    gcc-riscv-none-elf \
    git~2 \
    lld~17 \
    llvm18 \
    make~4 \
    mingw-w64-gcc~13 \
    mingw-w64-binutils~2 \
    nasm~2 \
    python3~3 \
    xorriso~1 && \
    ln -sf /usr/bin/clang-18 /usr/bin/clang && \
    ln -sf /usr/bin/clang++-18 /usr/bin/clang++ && \
    ln -sf /usr/bin/llvm18-ar /usr/bin/llvm-ar && \
    ln -sf /usr/bin/llvm18-objcopy /usr/bin/llvm-objcopy && \
    ln -sf /usr/bin/llvm18-ranlib /usr/bin/llvm-ranlib && \
    ln -sf /usr/bin/llvm18-strip /usr/bin/llvm-strip && \
    ln -sf /usr/bin/riscv-none-elf-gcc /usr/bin/riscv64-elf-gcc && \
    ln -sf /usr/bin/riscv-none-elf-ld /usr/bin/riscv64-elf-ld && \
    ln -sf /usr/bin/riscv-none-elf-objcopy /usr/bin/riscv64-elf-objcopy && \
    ln -sf /usr/bin/riscv-none-elf-strip /usr/bin/riscv64-elf-strip
