# Apheleia operating system (AOS)

`In Greek mythology, Apheleia (Ἀφέλεια) was the spirit and personification of ease, simplicity and primitivity in the good sense ...` - [Wikipedia](https://en.wikipedia.org/wiki/Apheleia)

### What is AOS?

Apheleia is a 64 bit UNIX like hobby operating system made for fun and as a learning opportunity.
It aims to be as minimalistic and simple as possible while still providing basic functionality.

### What does this repository include?

- the kernel source in `kernel/`
- eltorito/hdd bootloader in `boot/`
- basic libc implementation in `libs/libc` and `libs/libc_ext`
- simple logging library in `libs/log`
- ANSI escape code parser in `libs/term`
- basic data structures and related functions in `libs/data`
- bdf font to c header conversion awk script in `utils/bdf_parser.awk`
- Some other small miscellaneous libs with common functions/macros

### How to build and run?

Build under docker (*recommended*):

```bash
make docker_image docker_build
```

Build locally (on a linux machine):

```bash
make
```

After a successful build an iso image will be generated (`bin/apheleia.iso`).
You can run it using QEMU with (on a linux machine):

```bash
make run
```

### License

This entire repo is released under the terms on the GPLv3 (see `license`). Feel free to reuse, build upon or reference this code as long as your projects respect the GPL i.e. are free software themselves.

`~ Happy hacking :^)`
