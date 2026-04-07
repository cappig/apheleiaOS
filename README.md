# Apheleia operating system (AOS)

`In Greek mythology, Apheleia (Ἀφέλεια) was the spirit and personification of ease, simplicity and primitivity in the good sense ...` - [Wikipedia](https://en.wikipedia.org/wiki/Apheleia)

### What is AOS?

Apheleia is an x86 UNIX-like hobby operating system made for fun and as a learning opportunity.
It aims to be as minimalistic and simple as possible while still providing basic functionality.

The current tree supports `x86_64`, `x86_32`, `riscv_64`, and `riscv_32` builds, BIOS boot by default, optional x86_64 UEFI boot, early RISC-V bring-up, and a small windowed userland.

![Apheleia OS running](aos.png)

### What does this repository include?

- the kernel source in `kernel/`
- the userspace tree in `userland/{core,ui,tools,games}`
- the staged root filesystem content in `root/`
- the libc and support libraries in `libs/`
- image/QEMU/OVMF helpers in `utils/`
- some other small miscellaneous libs with common functions/macros

### How to build and run?

Build with Docker (*recommended for cross-platform builds*):

```bash
# Default build: x86_64
make docker_build

# 32-bit x86
make docker_build ARCH=x86_32

# RISC-V
make docker_build ARCH=riscv_32 TOOLCHAIN=llvm
make docker_build ARCH=riscv_64 TOOLCHAIN=llvm
```

`make docker_build` will build or refresh the Docker image automatically.

For RISC-V, use `TOOLCHAIN=llvm` in Docker just like you would locally.

If you want to prepare the image explicitly first:

```bash
make docker_image
```

Build locally on a Linux machine:

```bash
make
```

Useful local variants:

```bash
# 32-bit x86
make ARCH=x86_32

# RISC-V with LLVM
make ARCH=riscv_32 TOOLCHAIN=llvm
make ARCH=riscv_64 TOOLCHAIN=llvm

# Build an ISO instead of the default raw disk image
make IMAGE_FORMAT=iso
```

After a successful build, the disk image will be generated at:

```bash
bin/apheleia_alpha-1.1_<arch>.img
```

Run it with QEMU using the same `ARCH` you built:

```bash
make run
```

Useful variants:

```bash
# Run RISC-V 32-bit
make ARCH=riscv_32 run

# Run RISC-V 64-bit
make ARCH=riscv_64 run

# Run with 4 virtual CPUs
make run QEMU_SMP=4

# Use KVM when available
make run KVM=true

# x86_64 UEFI boot
make run BOOT=uefi
```

### License

This entire repo is released under the terms on the GPLv3 (see `license`). Feel free to reuse, build upon or reference this code as long as your projects respect the GPL i.e. are free software themselves.

`~ Happy hacking :^)`
