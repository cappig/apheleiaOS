#!/usr/bin/env python3
"""Boot an image under QEMU and check that userland still works.

The build only proves the tree compiles. This drives a real login shell over
the serial console and fails if any step stops behaving, which is what keeps a
change that builds but panics from looking healthy.
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

QEMU_FOR_ARCH = {
    "x86_64": "qemu-system-x86_64",
    "x86_32": "qemu-system-i386",
    "riscv_64": "qemu-system-riscv64",
    "riscv_32": "qemu-system-riscv32",
}

# (input, substring that must appear in the output it produces)
STEPS = [
    ("user\n", None),
    ("user\n", "$"),
    ("whoami\n", "user"),
    ("id\n", "uid=1000(user)"),
    ("groups\n", "wheel"),
    ("echo USER=$USER\n", "USER=user"),
    ("uname\n", "apheleiaOS"),
    ("ls /bin\n", "sh"),
    ("echo hi > /tmp/smoke && cat /tmp/smoke\n", "hi"),
    ("head -n 1 /etc/passwd\n", "root:"),
    ("cat /etc/shadow\n", "Permission denied"),
    ("for i in 1 2 3; do echo n$i; done\n", "n3"),
    ("if true; then echo yes; fi\n", "yes"),
    ("f() { echo fn-$1; }; f ok\n", "fn-ok"),
    ("case ab in a*) echo glob;; esac\n", "glob"),
    ("su\n", None),
    ("root\n", "#"),
    ("whoami\n", "root"),
    ("cat /etc/shadow\n", "root:"),
    ("exit\n", None),
    ("whoami\n", "user"),
]


def strip_ansi(text: str) -> str:
    return ANSI.sub("", text)


def wait_for_login(log: Path, deadline: float) -> bool:
    while time.monotonic() < deadline:
        if "login:" in log.read_text(errors="replace"):
            return True
        time.sleep(0.5)
    return False


def run(
    image: Path,
    arch: str,
    memory: str,
    step_delay: float,
    boot_timeout: float,
    verbose: bool,
) -> int:
    qemu = QEMU_FOR_ARCH[arch]
    log = ROOT / "bin" / f"smoke_{arch}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_bytes(b"")

    argv = [
        qemu,
        "-no-reboot",
        "-cpu", "max",
        "-m", memory,
        "-smp", "2",
        "-display", "none",
        "-serial", "stdio",
    ]
    if arch.startswith("x86"):
        argv += ["-drive", f"format=raw,file={image}"]
    else:
        # the raw image is loaded at the reset vector, matching `make run`
        loader = f"loader,file={image},addr=0x80000000,cpu-num=0,force-raw=on"
        argv += ["-machine", "virt", "-bios", "none", "-device", loader]

    with log.open("wb") as sink:
        proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=sink, stderr=subprocess.STDOUT)

        try:
            # polling for the prompt rather than sleeping a fixed time keeps the
            # test both quick on a fast host and reliable on a slow CI runner
            reached = wait_for_login(log, time.monotonic() + boot_timeout)
            if reached:
                for text, _ in STEPS:
                    proc.stdin.write(text.encode())
                    proc.stdin.flush()
                    time.sleep(step_delay)
        except BrokenPipeError:
            pass
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    output = strip_ansi(log.read_text(errors="replace"))
    if verbose:
        print(output)

    if "login:" not in output:
        print(f"smoke: {arch}: never reached the login prompt, see {log}", file=sys.stderr)
        return 1

    for pattern in ("panic", "Kernel panic", "unhandled exception"):
        if pattern.lower() in output.lower():
            print(f"smoke: {arch}: kernel reported '{pattern}', see {log}", file=sys.stderr)
            return 1

    missing = [expect for _, expect in STEPS if expect and expect not in output]
    if missing:
        for expect in missing:
            print(f"smoke: {arch}: expected output missing: {expect!r}", file=sys.stderr)
        print(f"smoke: {arch}: see {log}", file=sys.stderr)
        return 1

    print(f"smoke: {arch}: {len([s for s in STEPS if s[1]])} checks passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--arch", default="x86_64", choices=sorted(QEMU_FOR_ARCH))
    parser.add_argument("--memory", default="256M", help="guest RAM exposed by QEMU")
    parser.add_argument("--step-delay", type=float, help="seconds to wait after each command")
    parser.add_argument("--boot-timeout", type=float, help="seconds to wait for the login prompt")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if not args.image.exists():
        print(f"smoke: no such image: {args.image}", file=sys.stderr)
        return 1

    # emulated RISC-V has no host acceleration, so it runs a few times slower
    slow = not args.arch.startswith("x86")
    step_delay = args.step_delay if args.step_delay is not None else (2.0 if slow else 1.2)
    boot_timeout = args.boot_timeout if args.boot_timeout is not None else (90.0 if slow else 45.0)

    return run(args.image, args.arch, args.memory, step_delay, boot_timeout, args.verbose)


if __name__ == "__main__":
    raise SystemExit(main())
