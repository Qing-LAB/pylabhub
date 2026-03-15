"""Allow ``python -m pylabhub`` to print installation info."""

from __future__ import annotations

import pylabhub


def main() -> None:
    print(f"pyLabHub {pylabhub.__version__}")
    print()
    print(f"  Data dir:    {pylabhub.get_data_dir()}")
    print(f"  Binaries:    {pylabhub.get_bin_dir()}")
    print(f"  Libraries:   {pylabhub.get_lib_dir()}")
    print(f"  Headers:     {pylabhub.get_include_dir()}")
    print(f"  Share:       {pylabhub.get_share_dir()}")
    print(f"  Config:      {pylabhub.get_config_dir()}")

    python_dir = pylabhub.get_python_dir()
    if pylabhub.runtime_available():
        print(f"  Python 3.14: {python_dir} (installed)")
    else:
        print(f"  Python 3.14: {python_dir} (NOT installed)")
        print()
        print("  Run 'pylabhub prepare-runtime' to download the Python 3.14 runtime.")

    bin_dir = pylabhub.get_bin_dir()
    if bin_dir.exists():
        exes = sorted(p.name for p in bin_dir.iterdir() if p.is_file())
        if exes:
            print()
            print("  Installed executables:")
            for exe in exes:
                print(f"    - {exe}")
    else:
        print()
        print("  (data directory not found — binaries not installed)")


if __name__ == "__main__":
    main()
