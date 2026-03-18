"""
Download and install the standalone Python 3.14 runtime for pyLabHub.

The C++ executables (pylabhub-producer, pylabhub-consumer, etc.) embed
Python 3.14 for user-script callbacks.  To keep the PyPI wheel small,
the runtime is NOT bundled in the wheel --- users download it once after
install via ``pylabhub prepare-runtime``.

The archive is the same astral-sh/python-build-standalone release used
at CMake configure time, pinned by URL and SHA-256.
"""

from __future__ import annotations

import hashlib
import pathlib
import platform
import shutil
import sys
import tarfile
import tempfile
import urllib.request

# ---------------------------------------------------------------------------
# Pinned release — sourced from cmake/Versions.cmake via generated module.
# Falls back to hardcoded values if the generated module is not available
# (e.g., when installed from a wheel built without CMake configure).
# ---------------------------------------------------------------------------
try:
    from pylabhub._version_generated import PYTHON_RUNTIME_VERSION, PYTHON_RELEASE_TAG
    PYTHON_STANDALONE_VERSION = PYTHON_RUNTIME_VERSION
except ImportError:
    PYTHON_STANDALONE_VERSION = "3.14.3+20260211"
    PYTHON_RELEASE_TAG = "20260211"

BASE_URL = (
    f"https://github.com/astral-sh/python-build-standalone"
    f"/releases/download/{PYTHON_RELEASE_TAG}"
)

# (os_name, machine) -> {filename, sha256, sentinel}
PYTHON_BUILDS: dict[tuple[str, str], dict[str, str]] = {
    ("linux", "x86_64"): {
        "filename": f"cpython-{PYTHON_STANDALONE_VERSION}-x86_64-unknown-linux-gnu-install_only.tar.gz",
        "sha256": "a3917eee21b61c9d8bfab22a773d1fe6945683dd40b5d5b263527af2550e3bbf",
        "sentinel": "bin/python3",
    },
    ("darwin", "arm64"): {
        "filename": f"cpython-{PYTHON_STANDALONE_VERSION}-aarch64-apple-darwin-install_only.tar.gz",
        "sha256": "df38f57df6b1030375d854e01bf7d4080971a2946b029fe5e8e86ff70efa2216",
        "sentinel": "bin/python3",
    },
    ("darwin", "x86_64"): {
        "filename": f"cpython-{PYTHON_STANDALONE_VERSION}-x86_64-apple-darwin-install_only.tar.gz",
        "sha256": "f858d7c53b479bafd84812da79db061a7401fedd448deb65e81549728e4568f3",
        "sentinel": "bin/python3",
    },
    ("windows", "x86_64"): {
        "filename": f"cpython-{PYTHON_STANDALONE_VERSION}-x86_64-pc-windows-msvc-install_only.tar.gz",
        "sha256": "fcaae26be290da3c51fa14d0e89fe004b7858ed285038938b18e5682b7f7c592",
        "sentinel": "python.exe",
    },
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
_DATA_DIR = pathlib.Path(__file__).parent / "data"


def get_default_target() -> pathlib.Path:
    """Default install location: <site-packages>/pylabhub/data/opt/python/."""
    return _DATA_DIR / "opt" / "python"


def get_platform_key() -> tuple[str, str]:
    """Return (os_name, machine) matching PYTHON_BUILDS keys."""
    s = sys.platform
    if s.startswith("linux"):
        os_name = "linux"
    elif s == "darwin":
        os_name = "darwin"
    elif s == "win32":
        os_name = "windows"
    else:
        raise RuntimeError(f"Unsupported platform: {s}")

    machine = platform.machine().lower()
    # Normalise common aliases.
    if machine in ("amd64", "x86_64"):
        machine = "x86_64"
    elif machine in ("aarch64", "arm64"):
        machine = "arm64"

    return (os_name, machine)


def runtime_is_installed(target: pathlib.Path | None = None) -> bool:
    """True if the sentinel executable exists at *target*."""
    if target is None:
        target = get_default_target()
    try:
        key = get_platform_key()
        info = PYTHON_BUILDS[key]
    except (RuntimeError, KeyError):
        # Unknown platform --- cannot determine sentinel; assume not installed.
        return False
    return (target / info["sentinel"]).is_file()


def verify_sha256(filepath: pathlib.Path, expected: str) -> bool:
    """Stream-hash *filepath* and compare to *expected* (hex, lowercase)."""
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(1 << 20)  # 1 MiB
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest().lower() == expected.lower()


def _download_with_progress(url: str, dest: pathlib.Path) -> None:
    """Download *url* to *dest*, printing a progress bar to stderr."""

    def _reporthook(
        block_num: int, block_size: int, total_size: int
    ) -> None:
        downloaded = block_num * block_size
        if total_size > 0:
            pct = min(100.0, downloaded / total_size * 100)
            mb_done = downloaded / (1 << 20)
            mb_total = total_size / (1 << 20)
            print(
                f"\r  Downloading: {mb_done:6.1f} / {mb_total:.1f} MB ({pct:5.1f}%)",
                end="",
                flush=True,
                file=sys.stderr,
            )
        else:
            mb_done = downloaded / (1 << 20)
            print(
                f"\r  Downloading: {mb_done:6.1f} MB",
                end="",
                flush=True,
                file=sys.stderr,
            )

    urllib.request.urlretrieve(url, str(dest), reporthook=_reporthook)
    print(file=sys.stderr)  # newline after progress


def _extract_archive(archive: pathlib.Path, target: pathlib.Path) -> None:
    """Extract tar.gz to *target*.  Astral tarballs unpack to ``python/``."""
    # Extract into a temp dir on the same filesystem so we can atomic-rename.
    tmp = pathlib.Path(tempfile.mkdtemp(dir=target.parent))
    try:
        print(f"  Extracting to {target} ...", file=sys.stderr)
        with tarfile.open(archive, "r:gz") as tf:
            tf.extractall(path=tmp)

        extracted = tmp / "python"
        if not extracted.is_dir():
            # Fallback: maybe the tarball root IS the content.
            candidates = [d for d in tmp.iterdir() if d.is_dir()]
            if len(candidates) == 1:
                extracted = candidates[0]
            else:
                raise RuntimeError(
                    f"Unexpected archive layout: {list(tmp.iterdir())}"
                )

        # Atomic move to final location.
        if target.exists():
            shutil.rmtree(target)
        extracted.rename(target)
    finally:
        # Clean up temp dir (may be partially empty after rename).
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------------
def prepare_runtime(
    *,
    target: pathlib.Path | None = None,
    from_archive: pathlib.Path | None = None,
) -> None:
    """Download (or copy) and install the standalone Python 3.14 runtime.

    Parameters
    ----------
    target:
        Installation directory.  Defaults to ``<package>/data/opt/python/``.
    from_archive:
        Path to a pre-downloaded ``.tar.gz`` archive (offline/air-gapped).
        If ``None``, the archive is downloaded from GitHub.
    """
    if target is None:
        target = get_default_target()

    key = get_platform_key()
    if key not in PYTHON_BUILDS:
        print(
            f"Error: no Python build available for platform {key}.",
            file=sys.stderr,
        )
        sys.exit(1)

    info = PYTHON_BUILDS[key]

    if runtime_is_installed(target):
        print(
            f"Python {PYTHON_STANDALONE_VERSION} runtime is already installed at:\n"
            f"  {target}\n"
            f"\n"
            f"To reinstall, remove the directory first:\n"
            f"  rm -rf {target}",
        )
        return

    # Ensure parent directory exists.
    target.parent.mkdir(parents=True, exist_ok=True)

    if from_archive is not None:
        # Offline install from a local archive.
        if not from_archive.is_file():
            print(f"Error: archive not found: {from_archive}", file=sys.stderr)
            sys.exit(1)

        print(f"Verifying {from_archive.name} ...")
        if not verify_sha256(from_archive, info["sha256"]):
            print(
                f"Error: SHA-256 mismatch for {from_archive}.\n"
                f"Expected: {info['sha256']}\n"
                f"The archive may be corrupted or for a different platform.",
                file=sys.stderr,
            )
            sys.exit(1)

        _extract_archive(from_archive, target)
    else:
        # Online download.
        url = f"{BASE_URL}/{info['filename']}"
        print(f"Downloading Python {PYTHON_STANDALONE_VERSION} for {key[0]}/{key[1]} ...")
        print(f"  URL: {url}")

        # Download to a temp file, verify, then extract.
        tmp_dir = pathlib.Path(tempfile.mkdtemp())
        archive_path = tmp_dir / info["filename"]
        try:
            _download_with_progress(url, archive_path)

            print(f"  Verifying SHA-256 ...")
            if not verify_sha256(archive_path, info["sha256"]):
                print(
                    f"Error: SHA-256 mismatch after download.\n"
                    f"Expected: {info['sha256']}\n"
                    f"The download may be corrupted. Please try again.",
                    file=sys.stderr,
                )
                sys.exit(1)
            print(f"  SHA-256 OK.")

            _extract_archive(archive_path, target)
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    # Verify sentinel.
    sentinel = target / info["sentinel"]
    if not sentinel.is_file():
        print(
            f"Error: extraction succeeded but sentinel not found: {sentinel}",
            file=sys.stderr,
        )
        sys.exit(1)

    print()
    print(f"Python {PYTHON_STANDALONE_VERSION} runtime installed successfully.")
    print(f"  Location: {target}")
    print()
    print("pyLabHub is ready to use.")
