#!/usr/bin/env python3

import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".s", ".S", ".asm"}
BUILD_NAMES = {"Makefile", "makefile"}
BUILD_SUFFIXES = {".mk", ".sh"}

ARCH_MACRO = re.compile(r"__(?:riscv|i386|x86_64|x86)\b|\bARCH_NAME\b")
ARCH_BRANCH = re.compile(
    r"^\s*(?:else\s+)?ifn?eq\b.*(?:x86|riscv|RISCV|ARCH_TREE|ARCH_VARIANT)",
    re.MULTILINE,
)
SHELL_ARCH_BRANCH = re.compile(r"^\s*(?:if|elif)\b.*(?:x86|riscv|frisc)", re.IGNORECASE | re.MULTILINE)


def repo_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    return [ROOT / name.decode() for name in result.stdout.split(b"\0") if name]


def is_arch_file(path: Path) -> bool:
    return "arch" in path.relative_to(ROOT).parts


def main() -> int:
    failures: list[str] = []

    for path in repo_files():
        if not path.is_file() or is_arch_file(path):
            continue

        is_source = path.suffix in SOURCE_SUFFIXES
        is_build = path.name in BUILD_NAMES or path.suffix in BUILD_SUFFIXES
        if not is_source and not is_build:
            continue

        text = path.read_text(encoding="utf-8", errors="replace")
        if is_source:
            pattern = ARCH_MACRO
        elif path.suffix == ".sh":
            pattern = SHELL_ARCH_BRANCH
        else:
            pattern = ARCH_BRANCH

        for match in pattern.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            rel = path.relative_to(ROOT)
            failures.append(f"{rel}:{line}: architecture-specific code must live under arch/")

    if failures:
        print("\n".join(failures))
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
