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

# FRISC FPGA image
make docker_build ARCH=riscv_32 TOOLCHAIN=llvm RISCV_FRISC=true
```

`make docker_build` will build or refresh the Docker image automatically.

Docker also carries the cross tools needed for GNU RISC-V builds:

```bash
make docker_build ARCH=riscv_32 TOOLCHAIN=gnu RISCV_FRISC=true
```

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

# FRISC FPGA image
make ARCH=riscv_32 TOOLCHAIN=llvm RISCV_FRISC=true

# Build an ISO instead of the default raw disk image
make IMAGE_FORMAT=iso
```

After a successful build, the disk image will be generated at:

```bash
bin/apheleia_<version>_<arch>.img
```

Common build knobs:

- `ARCH=x86_64|x86_32|riscv_64|riscv_32`
- `TOOLCHAIN=gnu|llvm`
- `PROFILE=fast|normal|small|debug|debug_extra`
- `IMAGE_FORMAT=img|iso` (`iso` is for x86 images)
- `RISCV_FRISC=true` builds the FRISC FPGA image, sets the default UART stride to `4`, builds `kernel/arch/riscv/dts/friscv.dts`, and stages it as `/boot/platform.dtb`
- `RISCV_UART_STRIDE=<n>` overrides the RISC-V UART register stride manually
- `STRIP_USER_SYMBOLS=true` strips user binaries more aggressively

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

# Run RISC-V in Spike
make ARCH=riscv_32 TOOLCHAIN=llvm run-spike

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
