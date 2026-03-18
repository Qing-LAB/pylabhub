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

# Version from cmake/Versions.cmake (generated at configure time).
try:
    from pylabhub._version_generated import __version__
    from pylabhub._version_generated import PYTHON_RUNTIME_VERSION as python_runtime_version
    from pylabhub._version_generated import PYTHON_RELEASE_TAG as python_release_tag
except ImportError:
    __version__ = "0.1.0a0"
    python_runtime_version = "unknown"
    python_release_tag = "unknown"


def abi_versions() -> dict:
    """Query ABI/protocol versions from the installed C++ shared library.

    Loads ``libpylabhub-utils`` via ctypes and calls the C-linkage function
    ``pylabhub_abi_info_json()`` which returns a stable ``const char*``.

    Returns a dict with keys: release, library, python_runtime, shm_major,
    shm_minor, wire_major, wire_minor, script_api_major, script_api_minor,
    facade_producer, facade_consumer.

    Falls back to a minimal dict if the library is not available.
    """
    import ctypes
    import json
    import platform as plat
    import sys

    try:
        lib_dir = get_lib_dir()
        if sys.platform == "win32":
            lib_path = lib_dir / "pylabhub-utils.dll"
        elif plat.system() == "Darwin":
            lib_path = lib_dir / "libpylabhub-utils.dylib"
        else:
            lib_path = lib_dir / "libpylabhub-utils.so"

        if not lib_path.exists():
            return {"release": __version__, "python_runtime": python_runtime_version}

        lib = ctypes.CDLL(str(lib_path))
        lib.pylabhub_abi_info_json.restype = ctypes.c_char_p
        lib.pylabhub_abi_info_json.argtypes = []
        raw = lib.pylabhub_abi_info_json()
        if raw:
            return json.loads(raw.decode("utf-8"))
    except Exception:
        pass

    return {"release": __version__, "python_runtime": python_runtime_version}

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


def get_python_dir() -> pathlib.Path:
    """Directory containing the bundled Python 3.14 runtime."""
    return _DATA_DIR / "opt" / "python"


def runtime_available() -> bool:
    """True if the bundled Python 3.14 runtime is installed."""
    from pylabhub._runtime import runtime_is_installed

    return runtime_is_installed()
