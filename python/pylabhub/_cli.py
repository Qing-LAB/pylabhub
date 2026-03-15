"""
Console-script entry points for pyLabHub.

Top-level command:
    pylabhub                   -- info / subcommand dispatcher
    pylabhub prepare-runtime   -- download the bundled Python 3.14 runtime

Binary wrappers:
    pylabhub-hubshell, pylabhub-producer, pylabhub-consumer,
    pylabhub-processor, pylabhub-pyenv

Each binary wrapper replaces the current process with the corresponding
C++ executable using os.execvp (no subprocess overhead, proper signal
handling, correct exit code).
"""

from __future__ import annotations

import os
import sys

from pylabhub import get_bin_dir


# ---------------------------------------------------------------------------
# Binary wrappers
# ---------------------------------------------------------------------------
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

    # The C++ binaries embed Python and will fail without the runtime.
    from pylabhub._runtime import runtime_is_installed

    if not runtime_is_installed():
        print(
            "pylabhub: Python 3.14 runtime not found.\n"
            "\n"
            "The C++ binaries require the bundled Python runtime.\n"
            "Run the following command to download and install it:\n"
            "\n"
            "    pylabhub prepare-runtime\n"
            "\n"
            "For offline/air-gapped systems, download the archive first,\n"
            "then run:\n"
            "\n"
            "    pylabhub prepare-runtime --from /path/to/archive.tar.gz\n",
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


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------
def _cmd_prepare_runtime() -> None:
    """Handle ``pylabhub prepare-runtime``."""
    import argparse
    import pathlib

    parser = argparse.ArgumentParser(
        prog="pylabhub prepare-runtime",
        description="Download and install the Python 3.14 runtime for pyLabHub.",
    )
    parser.add_argument(
        "--target",
        type=str,
        default=None,
        help="Custom install directory (default: <package>/data/opt/python/)",
    )
    parser.add_argument(
        "--from",
        dest="from_archive",
        type=str,
        default=None,
        help="Path to a pre-downloaded .tar.gz archive (offline/air-gapped install)",
    )
    args = parser.parse_args(sys.argv[2:])

    from pylabhub._runtime import prepare_runtime

    target = pathlib.Path(args.target) if args.target else None
    from_archive = pathlib.Path(args.from_archive) if args.from_archive else None
    prepare_runtime(target=target, from_archive=from_archive)


# ---------------------------------------------------------------------------
# Top-level entry point: pylabhub <subcommand>
# ---------------------------------------------------------------------------
def main() -> None:
    """Entry point for the ``pylabhub`` console script."""
    if len(sys.argv) < 2 or sys.argv[1] in ("--help", "-h"):
        from pylabhub.__main__ import main as info_main

        info_main()

        if len(sys.argv) >= 2:
            print()
            print("Commands:")
            print("  prepare-runtime   Download and install the Python 3.14 runtime")
            print()
            print("Run 'pylabhub <command> --help' for command-specific help.")
        return

    subcommand = sys.argv[1]

    if subcommand == "prepare-runtime":
        _cmd_prepare_runtime()
    elif subcommand == "--version":
        import pylabhub

        print(f"pylabhub {pylabhub.__version__}")
    else:
        print(f"pylabhub: unknown command '{subcommand}'", file=sys.stderr)
        print("Run 'pylabhub --help' for available commands.", file=sys.stderr)
        sys.exit(1)
