#!/usr/bin/env python3
"""pylabhub-pyenv — Manage the pyLabHub bundled Python environment.

This tool manages the standalone Python distribution bundled with pyLabHub.
It controls package installation, verification, environment inspection, and
virtual environment management.

Usage (via wrapper scripts):
    pylabhub-pyenv install    [-r REQUIREMENTS] [--wheels-dir DIR] [--venv NAME]
    pylabhub-pyenv verify     [-r REQUIREMENTS] [--venv NAME]
    pylabhub-pyenv info       [--venv NAME]
    pylabhub-pyenv freeze     [--venv NAME]
    pylabhub-pyenv create-venv NAME
    pylabhub-pyenv list-venvs
    pylabhub-pyenv remove-venv NAME

The wrapper scripts (pylabhub-pyenv / pylabhub-pyenv.ps1) locate the bundled
Python interpreter automatically and invoke this script with it.  This script
must NOT be run with a system Python — it operates on the interpreter that
executes it (i.e., the bundled standalone Python).

Virtual environments are created under <python_home>/venvs/<name>/ and share
the base interpreter.  Role JSON configs can specify "python_venv": "<name>"
to activate a specific venv at runtime.
"""

from __future__ import annotations

import argparse
import importlib.metadata
import json
import platform
import shutil
import subprocess
import sys
import sysconfig
import venv
from pathlib import Path


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _prefix() -> Path:
    """Return the pyLabHub installation prefix.

    This script lives in ``<prefix>/bin/pylabhub-pyenv.py`` (staged) or
    ``<prefix>/tools/pylabhub-pyenv.py`` (source).  The prefix is one
    level up from the script's directory.
    """
    return Path(__file__).resolve().parent.parent


def _python_home() -> Path:
    """Return the root of the Python tree used by this interpreter.

    Derived from the stdlib path reported by sysconfig.
    On Unix:   opt/python/lib/python3.14  → go up 2
    On Windows: opt/python/Lib             → go up 1
    """
    stdlib = Path(sysconfig.get_path("stdlib"))
    if platform.system() == "Windows":
        return stdlib.parent
    return stdlib.parent.parent


def _venvs_dir() -> Path:
    """Return the directory where virtual environments are stored.

    Always ``<prefix>/opt/python/venvs/``.  This is a fixed location in the
    staging layout — user customization is via requirements.txt, not directory
    structure.
    """
    return _prefix() / "opt" / "python" / "venvs"


def _resolve_venv(name: str | None) -> Path | None:
    """Resolve a venv name to its directory, or None for base environment."""
    if not name:
        return None
    venv_dir = _venvs_dir() / name
    return venv_dir


def _venv_python(venv_dir: Path) -> Path:
    """Return the Python interpreter path inside a venv."""
    if platform.system() == "Windows":
        return venv_dir / "Scripts" / "python.exe"
    return venv_dir / "bin" / "python3"


def _venv_site_packages(venv_dir: Path) -> Path | None:
    """Return the site-packages directory inside a venv."""
    if platform.system() == "Windows":
        return venv_dir / "Lib" / "site-packages"
    # Unix: lib/python3.XX/site-packages
    lib_dir = venv_dir / "lib"
    if lib_dir.is_dir():
        for child in lib_dir.iterdir():
            if child.name.startswith("python") and child.is_dir():
                sp = child / "site-packages"
                if sp.is_dir():
                    return sp
    return None


def _pip_cmd(*extra: str, venv_dir: Path | None = None) -> list[str]:
    """Build a pip invocation list.

    If venv_dir is given, uses the venv's Python interpreter.
    Otherwise uses the base interpreter.
    """
    python = str(_venv_python(venv_dir)) if venv_dir else sys.executable
    return [python, "-m", "pip", *extra]


def _run(cmd: list[str], *, check: bool = True):
    """Run a subprocess, forwarding stdout/stderr."""
    return subprocess.run(cmd, check=check)


def _parse_requirements(path: Path) -> list[str]:
    """Parse a requirements.txt into a list of package names (without versions).

    Skips comments, blank lines, and option lines (``--find-links`` etc.).
    Handles ``pkg>=1.0``, ``pkg[extra]>=1.0``, ``pkg==1.0``, etc.
    """
    names: list[str] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith("-"):
            continue
        # Strip extras: numpy[extra]>=1.0 → numpy
        name = line.split("[")[0]
        # Strip version specifiers
        for sep in (">=", "<=", "==", "!=", "~=", ">", "<"):
            name = name.split(sep)[0]
        name = name.strip()
        if name:
            names.append(name.lower())
    return names


def _env_label(venv_name: str | None) -> str:
    """Human-readable label for the target environment."""
    return f"venv '{venv_name}'" if venv_name else "base environment"


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_install(args: argparse.Namespace) -> int:
    """Install packages from a requirements file."""
    req_path = Path(args.requirements).resolve()
    if not req_path.is_file():
        print(f"ERROR: requirements file not found: {req_path}", file=sys.stderr)
        return 1

    venv_dir = _resolve_venv(args.venv)
    if venv_dir and not venv_dir.is_dir():
        print(f"ERROR: venv '{args.venv}' does not exist. "
              f"Create it first: pylabhub-pyenv create-venv {args.venv}",
              file=sys.stderr)
        return 1

    print(f"=== pylabhub-pyenv install ({_env_label(args.venv)}) ===")
    print(f"  Python:       {sys.executable}")
    print(f"  Version:      {platform.python_version()}")
    print(f"  Python home:  {_python_home()}")
    if venv_dir:
        print(f"  Venv:         {venv_dir}")
    print(f"  Requirements: {req_path}")
    print()

    # Step 1: Upgrade pip, setuptools, wheel
    print("--- Upgrading pip, setuptools, wheel ---")
    upgrade_cmd = _pip_cmd("install", "--progress-bar", "off", "--upgrade",
                           "pip", "setuptools", "wheel", venv_dir=venv_dir)
    if args.wheels_dir:
        upgrade_cmd.extend(["--find-links", str(args.wheels_dir), "--no-index"])
    result = _run(upgrade_cmd, check=False)
    if result.returncode != 0:
        print("WARNING: pip/setuptools/wheel upgrade failed (continuing anyway)",
              file=sys.stderr)

    # Step 2: Install from requirements
    print()
    print(f"--- Installing packages from {req_path.name} ---")
    install_cmd = _pip_cmd("install", "--progress-bar", "off", "-r", str(req_path),
                           venv_dir=venv_dir)
    if args.wheels_dir:
        install_cmd.extend(["--find-links", str(args.wheels_dir), "--no-index"])
    result = _run(install_cmd, check=False)
    if result.returncode != 0:
        print(f"ERROR: pip install failed (exit code {result.returncode})",
              file=sys.stderr)
        return result.returncode

    # Step 3: Write stamp file
    stamp_dir = venv_dir if venv_dir else _python_home()
    stamp = stamp_dir / ".pip_env_ready"
    stamp.write_text(f"installed from {req_path}\n", encoding="utf-8")
    print()
    print(f"=== Install complete (stamp: {stamp}) ===")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    """Verify that all required packages are installed."""
    req_path = Path(args.requirements).resolve()
    if not req_path.is_file():
        print(f"ERROR: requirements file not found: {req_path}", file=sys.stderr)
        return 1

    venv_dir = _resolve_venv(args.venv)
    if venv_dir and not venv_dir.is_dir():
        print(f"ERROR: venv '{args.venv}' does not exist.", file=sys.stderr)
        return 1

    print(f"=== pylabhub-pyenv verify ({_env_label(args.venv)}) ===")
    if venv_dir:
        print(f"  Venv:         {venv_dir}")
    print(f"  Requirements: {req_path}")
    print()

    required = _parse_requirements(req_path)
    if not required:
        print("No packages to verify.")
        return 0

    if venv_dir:
        # For venvs, use pip list --format=json from the venv interpreter
        result = subprocess.run(
            _pip_cmd("list", "--format=json", venv_dir=venv_dir),
            capture_output=True, text=True, check=False
        )
        if result.returncode != 0:
            print(f"ERROR: pip list failed in venv '{args.venv}'", file=sys.stderr)
            return 1
        import json
        pip_list = json.loads(result.stdout)
        installed: dict[str, str] = {
            pkg["name"].lower(): pkg["version"] for pkg in pip_list
        }
    else:
        # For base environment, use importlib.metadata (faster, no subprocess)
        installed = {}
        for dist in importlib.metadata.distributions():
            name = dist.metadata["Name"]
            if name:
                installed[name.lower()] = dist.metadata["Version"]

    ok = True
    for pkg in required:
        # pip normalizes names: underscores ↔ hyphens
        normalized = pkg.replace("-", "_").replace(".", "_")
        version = installed.get(pkg) or installed.get(normalized)
        if not version:
            alt = pkg.replace("_", "-")
            version = installed.get(alt)
        if version:
            print(f"  OK   {pkg} ({version})")
        else:
            print(f"  FAIL {pkg} — NOT INSTALLED")
            ok = False

    print()
    if ok:
        print(f"=== All {len(required)} packages verified ===")
        return 0
    else:
        print("=== VERIFICATION FAILED — run 'pylabhub-pyenv install' ===",
              file=sys.stderr)
        return 1


def cmd_info(args: argparse.Namespace) -> int:
    """Display information about the Python environment."""
    py_home = _python_home()
    venv_dir = _resolve_venv(args.venv)

    if venv_dir and not venv_dir.is_dir():
        print(f"ERROR: venv '{args.venv}' does not exist.", file=sys.stderr)
        return 1

    print(f"=== pylabhub-pyenv info ({_env_label(args.venv)}) ===")
    print(f"  Base interpreter: {sys.executable}")
    print(f"  Version:          {platform.python_version()}")
    print(f"  Platform:         {platform.platform()}")
    print(f"  Python home:      {py_home}")

    if venv_dir:
        print(f"  Venv:             {venv_dir}")
        print(f"  Venv Python:      {_venv_python(venv_dir)}")
        sp = _venv_site_packages(venv_dir)
        print(f"  Site-packages:    {sp}")
        stamp = venv_dir / ".pip_env_ready"
    else:
        print(f"  Site-packages:    {sysconfig.get_path('purelib')}")
        print(f"  Prefix:           {sys.prefix}")
        print(f"  Base prefix:      {sys.base_prefix}")
        stamp = py_home / ".pip_env_ready"

    if stamp.is_file():
        print(f"  Env stamp:        {stamp} (exists)")
    else:
        print(f"  Env stamp:        {stamp} (MISSING — run 'pylabhub-pyenv install')")

    if not venv_dir:
        count = sum(1 for _ in importlib.metadata.distributions())
        print(f"  Packages:         {count} installed")

    # List available venvs
    venvs_root = _venvs_dir()
    if venvs_root.is_dir():
        venvs = sorted(d.name for d in venvs_root.iterdir() if d.is_dir())
        if venvs:
            print(f"  Available venvs:  {', '.join(venvs)}")

    return 0


def cmd_freeze(args: argparse.Namespace) -> int:
    """Print pip freeze output (installed packages and versions)."""
    venv_dir = _resolve_venv(args.venv)
    if venv_dir and not venv_dir.is_dir():
        print(f"ERROR: venv '{args.venv}' does not exist.", file=sys.stderr)
        return 1
    return _run(_pip_cmd("freeze", venv_dir=venv_dir), check=False).returncode


def cmd_create_venv(args: argparse.Namespace) -> int:
    """Create a new virtual environment."""
    name = args.name
    venv_dir = _venvs_dir() / name

    if venv_dir.exists():
        print(f"ERROR: venv '{name}' already exists at {venv_dir}", file=sys.stderr)
        print("Use 'pylabhub-pyenv remove-venv' first to recreate.", file=sys.stderr)
        return 1

    print(f"=== pylabhub-pyenv create-venv ===")
    print(f"  Name:         {name}")
    print(f"  Location:     {venv_dir}")
    print(f"  Base Python:  {sys.executable} ({platform.python_version()})")
    print()

    # Create the venv using stdlib venv module.
    # system_site_packages=False: isolated from base environment.
    # with_pip=True: bootstrap pip into the venv.
    print("--- Creating virtual environment ---")
    builder = venv.EnvBuilder(
        system_site_packages=False,
        clear=False,
        with_pip=True,
        symlinks=(platform.system() != "Windows"),
    )
    builder.create(str(venv_dir))

    venv_py = _venv_python(venv_dir)
    if not venv_py.exists():
        print(f"ERROR: venv creation succeeded but interpreter not found at {venv_py}",
              file=sys.stderr)
        return 1

    print(f"  Interpreter:  {venv_py}")
    print()
    print(f"=== venv '{name}' created ===")
    print()
    print("To install packages into this venv:")
    print(f"  pylabhub-pyenv install --venv {name} -r requirements.txt")
    print()
    print("To use this venv in a role config (JSON):")
    print(f'  "python_venv": "{name}"')
    return 0


def cmd_list_venvs(args: argparse.Namespace) -> int:
    """List all virtual environments."""
    venvs_root = _venvs_dir()

    print(f"=== pylabhub-pyenv list-venvs ===")
    print(f"  Venvs dir: {venvs_root}")
    print()

    if not venvs_root.is_dir():
        print("  (no venvs directory — none created yet)")
        return 0

    venvs = sorted(d for d in venvs_root.iterdir() if d.is_dir())
    if not venvs:
        print("  (no virtual environments)")
        return 0

    for v in venvs:
        py = _venv_python(v)
        stamp = v / ".pip_env_ready"
        status = "ready" if stamp.is_file() else "no packages installed"
        exists = "OK" if py.exists() else "BROKEN"
        print(f"  {v.name:20s}  [{exists}]  ({status})")

    print()
    print(f"  Total: {len(venvs)} venv(s)")
    return 0


def cmd_remove_venv(args: argparse.Namespace) -> int:
    """Remove a virtual environment."""
    name = args.name
    venv_dir = _venvs_dir() / name

    if not venv_dir.is_dir():
        print(f"ERROR: venv '{name}' does not exist.", file=sys.stderr)
        return 1

    print(f"=== pylabhub-pyenv remove-venv ===")
    print(f"  Removing: {venv_dir}")

    shutil.rmtree(venv_dir)

    print(f"  Done — venv '{name}' removed.")
    return 0


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def _default_requirements() -> str:
    """Return the default requirements.txt path relative to the installation.

    Layout: <prefix>/bin/pylabhub-pyenv  (or <prefix>/tools/pylabhub-pyenv.py)
            <prefix>/share/scripts/python/requirements.txt
    """
    script_dir = Path(__file__).resolve().parent
    candidate = script_dir.parent / "share" / "scripts" / "python" / "requirements.txt"
    if candidate.is_file():
        return str(candidate)
    return "requirements.txt"


def _add_venv_arg(parser: argparse.ArgumentParser) -> None:
    """Add the --venv argument to a subparser."""
    parser.add_argument("--venv", default=None, metavar="NAME",
                        help="Target a named virtual environment instead of the "
                             "base environment")


def _add_requirements_arg(parser: argparse.ArgumentParser) -> None:
    """Add the -r/--requirements argument to a subparser."""
    parser.add_argument("-r", "--requirements", default=_default_requirements(),
                        help="Path to requirements.txt "
                             "(default: share/scripts/python/requirements.txt)")


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="pylabhub-pyenv",
        description="Manage the pyLabHub bundled Python environment.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # --- install ---
    p_install = sub.add_parser("install",
                               help="Install packages from requirements.txt")
    _add_requirements_arg(p_install)
    _add_venv_arg(p_install)
    p_install.add_argument("--wheels-dir", default=None,
                           help="Directory with pre-downloaded .whl files "
                                "(enables offline install)")
    p_install.set_defaults(func=cmd_install)

    # --- verify ---
    p_verify = sub.add_parser("verify",
                              help="Verify all required packages are installed")
    _add_requirements_arg(p_verify)
    _add_venv_arg(p_verify)
    p_verify.set_defaults(func=cmd_verify)

    # --- info ---
    p_info = sub.add_parser("info",
                            help="Display Python environment information")
    _add_venv_arg(p_info)
    p_info.set_defaults(func=cmd_info)

    # --- freeze ---
    p_freeze = sub.add_parser("freeze",
                              help="List installed packages (pip freeze)")
    _add_venv_arg(p_freeze)
    p_freeze.set_defaults(func=cmd_freeze)

    # --- create-venv ---
    p_create = sub.add_parser("create-venv",
                              help="Create a new virtual environment")
    p_create.add_argument("name", help="Name of the virtual environment")
    p_create.set_defaults(func=cmd_create_venv)

    # --- list-venvs ---
    p_list = sub.add_parser("list-venvs",
                            help="List all virtual environments")
    p_list.set_defaults(func=cmd_list_venvs)

    # --- remove-venv ---
    p_remove = sub.add_parser("remove-venv",
                              help="Remove a virtual environment")
    p_remove.add_argument("name", help="Name of the virtual environment to remove")
    p_remove.set_defaults(func=cmd_remove_venv)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
