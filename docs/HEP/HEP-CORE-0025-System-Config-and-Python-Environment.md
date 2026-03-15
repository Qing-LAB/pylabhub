# HEP-CORE-0025: System Configuration and Python Environment

| Field     | Value                                            |
|-----------|--------------------------------------------------|
| Status    | Implemented                                      |
| Created   | 2026-03-14                                       |
| Updated   | 2026-03-14                                       |
| Author    | pyLabHub team                                    |
| Requires  | HEP-CORE-0011 (ScriptHost), HEP-CORE-0018 (Producer/Consumer) |

## 1. Overview

This HEP defines two related subsystems:

1. **System configuration** (`config/pylabhub.json`) — a global runtime config file
   that controls installation-level settings independent of any particular role.
2. **Python environment management** — how the bundled Python distribution is deployed,
   how packages are installed, and how virtual environments enable per-role customization.

## 2. Motivation

pyLabHub embeds a standalone Python interpreter (from `astral-sh/python-build-standalone`)
for cross-platform reproducibility.  However:

- Some platforms (e.g., FreeBSD) lack standalone builds and must use system Python.
- Different roles may need different Python packages (e.g., a consumer needs `zarr` but
  a producer only needs `numpy`).
- Post-installation package updates must be possible without rebuilding.

A system config file provides the flexibility to handle these cases without
recompilation, while virtual environments enable per-role package isolation.

## 3. System Configuration

### 3.1 File Location

```
<prefix>/config/pylabhub.json
```

Where `<prefix>` is the installation root (the staging directory or install prefix).
In a staged build, this is `build/stage-<buildtype>/`.

### 3.2 Schema

```json
{
    "python_home": "/usr/local"
}
```

| Field          | Type   | Default          | Description |
|----------------|--------|------------------|-------------|
| `python_home`  | string | (auto-detect)    | Path to the Python installation root. Relative paths are resolved relative to `<prefix>`. When absent, falls back to `<prefix>/opt/python/`. |

**Future fields** (not yet implemented — reserved for forward compatibility):

| Field              | Type   | Description |
|--------------------|--------|-------------|
| `log_dir`          | string | Default log output directory for all roles |
| `default_broker`   | string | Default broker endpoint when hub_dir is absent |
| `license_file`     | string | Path to license file for commercial deployments |

### 3.3 Resolution Order

The Python home directory is resolved by `PythonScriptHost::do_initialize()` in
priority order:

1. **`$PYLABHUB_PYTHON_HOME`** environment variable (explicit override, useful for
   debugging and CI).
2. **`config/pylabhub.json` → `"python_home"`** (system config file).
3. **`<prefix>/opt/python/`** (standalone default — no config needed).

If none of the above yield a valid directory, the process exits with a clear error
message listing all checked locations.

Relative paths in tiers 1 and 2 are resolved relative to `<prefix>`.

### 3.4 When the Config File is Needed

| Scenario | Config file needed? |
|----------|-------------------|
| Standalone build (Linux/macOS/Windows) | No — `opt/python/` auto-discovery works |
| System Python (FreeBSD, custom builds) | Yes — set `"python_home"` to system prefix |
| CI with custom Python | Set `$PYLABHUB_PYTHON_HOME` instead |

## 4. Python Environment Architecture

### 4.1 Directory Layout

```
<prefix>/
  opt/
    python/                    ← standalone Python interpreter (PYTHONHOME)
      bin/python3              ← interpreter binary (Unix)
      python.exe               ← interpreter binary (Windows)
      lib/python3.14/
        site-packages/         ← base environment packages (numpy, zarr, etc.)
      venvs/                   ← virtual environments
        my-env/
          lib/python3.14/
            site-packages/     ← venv-specific packages
  bin/
    pylabhub-pyenv             ← environment management tool (Unix)
    pylabhub-pyenv.py          ← core logic (cross-platform)
    pylabhub-pyenv.ps1         ← environment management tool (Windows)
  config/
    pylabhub.json              ← system config (optional)
  share/
    scripts/python/
      requirements.txt         ← base environment package list
```

### 4.2 Two-Tier Package Model

**Base environment** (`opt/python/lib/python3.XX/site-packages/`):
- Installed at build time by cmake (`prepare_python_env` target).
- Controlled by `share/scripts/python/requirements.txt`.
- Contains the standard scientific stack: numpy, zarr, h5py, pyarrow, pyzmq, etc.
- All roles get these packages by default with zero configuration.

**Virtual environments** (`opt/python/venvs/<name>/`):
- Created post-installation via `pylabhub-pyenv create-venv <name>`.
- Packages installed via `pylabhub-pyenv install --venv <name> -r custom.txt`.
- Activated per-role via `"python_venv": "<name>"` in the role JSON config.
- Venv packages **overlay** base packages — if a venv provides numpy 2.1 and the
  base has numpy 2.0, the venv version wins.

### 4.3 Build-Time Package Installation

CMake options controlling the build-time pip install:

| Option | Default | Description |
|--------|---------|-------------|
| `PYLABHUB_PREPARE_PYTHON_ENV` | `ON` | Run pip install during build |
| `PYLABHUB_PYTHON_REQUIREMENTS_FILE` | `share/scripts/python/requirements.txt` | Requirements file |
| `PYLABHUB_PYTHON_WHEELS_DIR` | (empty) | Offline wheel directory |
| `PYLABHUB_PYTHON_LOCAL_ARCHIVE` | (empty) | Local Python archive for air-gapped builds |

### 4.4 Post-Installation Package Management

The `pylabhub-pyenv` tool manages the Python environment after installation:

```bash
# Base environment
pylabhub-pyenv install              # Install from default requirements.txt
pylabhub-pyenv install -r custom.txt  # Install from custom requirements
pylabhub-pyenv verify               # Check all required packages present
pylabhub-pyenv info                 # Show Python version, paths, package count
pylabhub-pyenv freeze               # pip freeze output

# Virtual environments
pylabhub-pyenv create-venv my-env
pylabhub-pyenv install --venv my-env -r custom.txt
pylabhub-pyenv verify --venv my-env -r custom.txt
pylabhub-pyenv list-venvs
pylabhub-pyenv remove-venv my-env

# Offline install (air-gapped deployments)
pylabhub-pyenv install --wheels-dir /path/to/wheels -r requirements.txt
```

### 4.5 Role Configuration: `python_venv`

Each role config (producer.json, consumer.json, processor.json) supports an
optional `"python_venv"` field:

```json
{
    "producer": { "uid": "PROD-SENSOR-AABB", "name": "TempSensor" },
    "channel": "lab.sensors.temp",
    "python_venv": "sensor-env",
    "script": { "path": "." }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `python_venv` | string | `""` (empty) | Name of the venv to activate. Empty = base environment only. |

At interpreter startup, `PythonScriptHost::do_initialize()`:
1. Sets `PYTHONHOME` to the base Python installation (stdlib works).
2. If `python_venv` is non-empty, calls `site.addsitedir(<venvs>/<name>/site-packages)`
   to overlay venv packages onto `sys.path`.

This is the standard Python venv activation mechanism — `PYTHONHOME` stays
pointing at the base interpreter while venv packages take precedence.

## 5. C++ Implementation

### 5.1 Python Home Resolution

`python_script_host.cpp` → `resolve_python_home()`:
- Static function, called at the start of `do_initialize()`.
- Implements the 3-tier priority (env → config → standalone).
- Reads `config/pylabhub.json` via nlohmann_json (already a dependency).
- Returns validated `fs::path`; throws on failure.

### 5.2 Virtual Environment Activation

After `py::scoped_interpreter` is created:
1. If `python_venv_` (set by subclass from config) is non-empty:
2. Resolve venvs dir: `<prefix>/opt/python/venvs/`.
3. Validate `<venvs_dir>/<name>/` exists.
4. Find `site-packages/` (platform-specific scan).
5. Call `site.addsitedir(site_packages)` via pybind11.

### 5.3 Config Field Propagation

Each role config struct has `std::string python_venv`.
Each role script host's `set_config()` copies it to `PythonScriptHost::python_venv_`.

## 6. Cross-Platform Support

| Platform | Python source | Config needed? | Notes |
|----------|--------------|----------------|-------|
| Linux x86_64 | python-build-standalone | No | Standalone in opt/python/ |
| Linux aarch64 | python-build-standalone | No | Standalone in opt/python/ |
| macOS x86_64 | python-build-standalone | No | Standalone in opt/python/ |
| macOS aarch64 | python-build-standalone | No | Standalone in opt/python/ |
| Windows x86_64 | python-build-standalone | No | Standalone in opt/python/ |
| FreeBSD | System Python | Yes | Set python_home in config |
| Other | System Python | Yes | Set python_home in config |

For system Python deployments:
- `python.cmake` skips the standalone download (existing `else()` clause).
- `find_package(Python)` discovers the system Python for pybind11 compilation.
- Admin creates `config/pylabhub.json` with `"python_home"` pointing at the
  system Python prefix.
- RPATH may need manual configuration (system lib directory).

## 7. pylabhub-pyenv Tool

### 7.1 Files

| File | Purpose |
|------|---------|
| `tools/pylabhub-pyenv.py` | Core logic (Python, cross-platform) |
| `tools/pylabhub-pyenv` | Bash wrapper (Linux/macOS) |
| `tools/pylabhub-pyenv.ps1` | PowerShell wrapper (Windows) |

All three are staged to `<prefix>/bin/` alongside the main executables.

### 7.2 Wrapper Resolution

The wrapper scripts locate the Python interpreter:
1. `$PYLABHUB_PYTHON` environment variable (explicit override).
2. `<script_dir>/../opt/python/bin/python3` (standalone default).

### 7.3 Requirements File Convention

The default requirements file is `share/scripts/python/requirements.txt`.
The tool auto-discovers it relative to its own location in the staging layout.
