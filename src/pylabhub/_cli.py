"""
Console-script entry points that delegate to the real C++ binaries.

Each function replaces the current process with the corresponding
pyLabHub executable using os.execvp, so the binary runs directly
(no subprocess overhead, proper signal handling, correct exit code).
"""

from __future__ import annotations

import os
import sys

from pylabhub import get_bin_dir


def _exec_binary(name: str) -> None:
    binary = get_bin_dir() / name
    if not binary.exists():
        print(
            f"pylabhub: {name} not found at {binary}\n"
            f"The pyLabHub C++ binaries may not be installed correctly.\n"
            f"Reinstall with: pip install --force-reinstall pylabhub",
            file=sys.stderr,
        )
        sys.exit(1)
    os.execvp(str(binary), [str(binary)] + sys.argv[1:])


def hubshell() -> None:
    _exec_binary("pylabhub-hubshell")


def producer() -> None:
    _exec_binary("pylabhub-producer")


def consumer() -> None:
    _exec_binary("pylabhub-consumer")


def processor() -> None:
    _exec_binary("pylabhub-processor")


def pyenv() -> None:
    _exec_binary("pylabhub-pyenv")
