FROM alpine:3.20

WORKDIR /usr/src/apheleia

RUN apk add --no-cache \
    binutils~2 \
    clang18 \
    curl~8 \
    gcc~13 \
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
    ln -sf /usr/bin/clang++-18 /usr/bin/clang++
