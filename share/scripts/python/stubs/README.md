# pyLabHub Python Type Stubs

PEP 561 type stubs for the four embedded pybind11 modules:

- `pylabhub_producer` — Producer-side scripting API
- `pylabhub_consumer` — Consumer-side scripting API
- `pylabhub_processor` — Processor-side scripting API
- `pylabhub_hub` — Hub-side scripting API (HEP-CORE-0033 §12.3)

These stubs give your editor + type checker (mypy, pyright,
Pylance) visibility into the API surface exposed by `plh_role` /
`plh_hub` to embedded Python scripts.  Without them, every
`import pylabhub_producer` is opaque — no autocomplete, no
type checking.

## Why hand-authored

The four modules are `PYBIND11_EMBEDDED_MODULE` — they exist only
inside the embedded interpreter of `plh_role` / `plh_hub`.  Tools
like `pybind11-stubgen` need to *run* the module to introspect it;
that requires linking the whole framework.  Hand-authored stubs
keep the workflow simple and don't pin the dev tooling.

Stubs MUST be kept in sync with the C++ bindings:

| Stub file | C++ source |
|-----------|------------|
| `pylabhub_producer-stubs/__init__.pyi` | `src/producer/producer_api.cpp` |
| `pylabhub_consumer-stubs/__init__.pyi` | `src/consumer/consumer_api.cpp` |
| `pylabhub_processor-stubs/__init__.pyi` | `src/processor/processor_api.cpp` |
| `pylabhub_hub-stubs/__init__.pyi` | `src/scripting/hub_api_python.cpp` |

## How to use

### Option A — set `MYPYPATH` / `PYTHONPATH`

```bash
# Point mypy / pyright at the install location:
export MYPYPATH=/path/to/install/share/scripts/python/stubs
# Or for pyright in VS Code, add to .vscode/settings.json:
#   "python.analysis.stubPath": "/path/to/install/share/scripts/python/stubs"
```

### Option B — install into your project's site-packages

```bash
pip install --target ~/.venv/lib/python3.X/site-packages \
    /path/to/install/share/scripts/python/stubs/pylabhub_producer-stubs
```

PEP 561 stub packages have a `-stubs` suffix and a `py.typed`
marker.  mypy/pyright pick them up automatically.

### Option C — vendor into your plugin repo

Copy the relevant `*-stubs/` directories into a `stubs/` folder
next to your script, then add to `mypy.ini`:

```ini
[mypy]
mypy_path = stubs
```

## Authoring tip

When the C++ binding adds a method, add it here too — same name,
matching argument types.  Run `mypy` (or your IDE's checker) on
the demos under `share/py-demo-*/` to validate that the stubs
match real usage.
