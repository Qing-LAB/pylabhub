"""
pyLabHub — High-performance IPC framework for scientific data acquisition.

This package provides path accessors for the installed C++ binaries and
runtime.  The actual executables (pylabhub-hubshell, pylabhub-producer,
pylabhub-consumer, pylabhub-processor) are prebuilt C++ programs located
under the ``data/`` subdirectory of this package.

Quick start::

    import pylabhub
    print(pylabhub.get_bin_dir())   # path to executables
    print(pylabhub.get_lib_dir())   # path to shared libraries
"""

from __future__ import annotations

import pathlib

__version__ = "0.1.0a0"

_DATA_DIR = pathlib.Path(__file__).parent / "data"


def get_data_dir() -> pathlib.Path:
    """Root of the installed pyLabHub data tree."""
    return _DATA_DIR


def get_bin_dir() -> pathlib.Path:
    """Directory containing pyLabHub executables."""
    return _DATA_DIR / "bin"


def get_lib_dir() -> pathlib.Path:
    """Directory containing shared libraries (libpylabhub-utils)."""
    return _DATA_DIR / "lib"


def get_include_dir() -> pathlib.Path:
    """Directory containing C++ public headers."""
    return _DATA_DIR / "include"


def get_share_dir() -> pathlib.Path:
    """Directory containing demos, examples, and utility scripts."""
    return _DATA_DIR / "share"


def get_config_dir() -> pathlib.Path:
    """Directory containing default configuration files."""
    return _DATA_DIR / "config"
