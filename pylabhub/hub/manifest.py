"""
pyLabHub manifest helper
========================

Utilities to create, read, validate, and finalize per-run manifests
(see README "Run manifest (manifest.yaml) — Draft schema").

Design goals:
- No heavy dependencies. YAML I/O uses PyYAML if available; otherwise JSON is supported.
- Deterministic hashing for integrity:
  - File SHA-256 for single files
  - Directory tree SHA-256 for Zarr stores (hash of sorted relative paths + content)
  - Self-hash of the manifest file with the `integrity.manifest_sha256` field blanked
- Minimal validation with helpful error messages (no jsonschema dependency).

CLI usage (temporary):
    python -m pylabhub.hub.manifest init --run-dir runs/run-2025-09-18-001 \
        --run-id run-2025-09-18-001 --description "NI-DAQ step response test"

    python -m pylabhub.hub.manifest finalize --run-dir runs/run-2025-09-18-001 \
        --manifest runs/run-2025-09-18-001/manifest.yaml

    python -m pylabhub.hub.manifest validate --manifest runs/run-2025-09-18-001/manifest.yaml

This module is deliberately conservative and easy to replace later with a richer
schema validator.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional
import argparse
import hashlib
import io
import json
import os
import sys
from pathlib import Path
from datetime import datetime, timezone

try:
    import yaml  # type: ignore
except Exception:  # pragma: no cover - optional
    yaml = None  # pyright: ignore[reportAssignmentType]


def _load_yaml_or_json(path: Path) -> Dict[str, Any]:
    if path.suffix.lower() in {".yaml", ".yml"}:
        if yaml is None:
            raise RuntimeError(
                f"PyYAML not installed; cannot read YAML file {path}. Install pyyaml or use JSON."
            )
        with path.open("r", encoding="utf-8") as f:
            return yaml.safe_load(f) or {}
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def _dump_yaml_or_json(data: Dict[str, Any], path: Path) -> None:
    if path.suffix.lower() in {".yaml", ".yml"}:
        if yaml is None:
            raise RuntimeError(
                f"PyYAML not installed; cannot write YAML file {path}. Install pyyaml or use JSON."
            )
        with path.open("w", encoding="utf-8") as f:
            yaml.safe_dump(data, f, sort_keys=False)
        return
    with path.open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def sha256_file(path: Path, chunk_size: int = 4 * 1024 * 1024) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(chunk_size), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_dir_tree(root: Path) -> str:
    h = hashlib.sha256()
    root = root.resolve()
    files: List[Path] = []
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            p = Path(dirpath) / name
            if p.is_symlink():
                continue
            files.append(p)
    files.sort(key=lambda p: p.relative_to(root).as_posix())
    for p in files:
        rel = p.relative_to(root).as_posix().encode("utf-8")
        h.update(rel)
        h.update(b"\x00")
        h.update(bytes.fromhex(sha256_file(p)))
    return h.hexdigest()


MIN_SCHEMA_VERSION = "0.1"


def _utcnow_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def init_manifest(
    run_id: str,
    description: str = "",
    investigator: str = "Alex Researcher",
    organization: str = "Example University",
) -> Dict[str, Any]:
    return {
        "schema_version": MIN_SCHEMA_VERSION,
        "run": {
            "id": run_id,
            "created_at": _utcnow_iso(),
            "investigator": investigator,
            "organization": organization,
            "award_number": None,
            "description": description,
        },
        "identifiers": {
            "dataset_pid": None,
            "orcids": [],
            "ror": None,
        },
        "acquisition": {
            "timebase": {
                "source_clock_hz": None,
                "hub_clock_offset_s": None,
            },
            "instruments": [],
            "streams": [],
        },
        "provenance": {
            "software": {},
            "recipe": {"name": None, "parameters": {}},
        },
        "integrity": {
            "manifest_sha256": None,
            "size_bytes_total": None,
            "checks": {"last_verified_at": None, "ok": None},
        },
        "sharing": {
            "access": "open",
            "embargo_until": None,
            "license": "CC-BY-4.0",
            "notes": None,
        },
    }


REQUIRED_KEYS = {
    "schema_version": str,
    "run": dict,
    "acquisition": dict,
    "integrity": dict,
}


def validate_manifest(m):
    errors = []
    for key, typ in REQUIRED_KEYS.items():
        if key not in m:
            errors.append(f"missing top-level key: {key}")
        elif not isinstance(m[key], typ):
            errors.append(f"{key} must be {typ.__name__}")
    run = m.get("run", {})
    for k in ("id", "created_at"):
        if not run.get(k):
            errors.append(f"run.{k} is required")
    acq = m.get("acquisition", {})
    streams = acq.get("streams", [])
    if not isinstance(streams, list):
        errors.append("acquisition.streams must be a list")
    else:
        for i, s in enumerate(streams):
            if not isinstance(s, dict):
                errors.append(f"streams[{i}] must be a dict")
                continue
            for req in ("name", "kind", "format", "path"):
                if not s.get(req):
                    errors.append(f"streams[{i}].{req} is required")
    integ = m.get("integrity", {})
    if "checks" in integ and not isinstance(integ["checks"], dict):
        errors.append("integrity.checks must be a dict")
    return errors


def _dir_size_bytes(p: Path) -> int:
    total = 0
    for dirpath, _, filenames in os.walk(p):
        for name in filenames:
            fp = Path(dirpath) / name
            if fp.is_symlink():
                continue
            try:
                total += fp.stat().st_size
            except FileNotFoundError:
                pass
    return total


def update_stream_hashes(manifest, run_dir: Path) -> None:
    acq = manifest.get("acquisition", {})
    streams = acq.get("streams", [])
    for s in streams:
        path = run_dir / Path(s["path"])  # type: ignore[index]
        if not path.exists():
            continue
        if path.is_dir():
            s["sha256"] = sha256_dir_tree(path)
        else:
            s["sha256"] = sha256_file(path)


def update_total_size(manifest, run_dir: Path) -> None:
    total = 0
    acq = manifest.get("acquisition", {})
    for s in acq.get("streams", []):
        p = run_dir / Path(s["path"])  # type: ignore[index]
        if p.exists():
            total += _dir_size_bytes(p) if p.is_dir() else p.stat().st_size
    manifest.setdefault("integrity", {})["size_bytes_total"] = int(total)


def _self_hash_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def _dump_without_manifest_hash(manifest) -> bytes:
    m2 = json.loads(json.dumps(manifest))
    m2.setdefault("integrity", {})["manifest_sha256"] = None
    s = json.dumps(m2, sort_keys=True, separators=(",", ":"))
    return s.encode("utf-8")


def finalize_manifest(manifest_path: Path, run_dir: Optional[Path] = None) -> None:
    manifest = _load_yaml_or_json(manifest_path)
    if run_dir is None:
        run_dir = manifest_path.parent
    manifest.setdefault("integrity", {}).setdefault("checks", {})["last_verified_at"] = _utcnow_iso()
    update_stream_hashes(manifest, run_dir)
    update_total_size(manifest, run_dir)
    self_hash = _self_hash_bytes(_dump_without_manifest_hash(manifest))
    manifest.setdefault("integrity", {})["manifest_sha256"] = self_hash
    _dump_yaml_or_json(manifest, manifest_path)


def read(path):
    return _load_yaml_or_json(Path(path))


def write(manifest, path):
    _dump_yaml_or_json(manifest, Path(path))


def _cmd_init(args):
    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    m = init_manifest(run_id=args.run_id, description=args.description)
    out = run_dir / (args.manifest or "manifest.yaml")
    write(m, out)
    print(f"Initialized manifest at {out}")
    return 0


def _cmd_finalize(args):
    finalize_manifest(Path(args.manifest), run_dir=Path(args.run_dir) if args.run_dir else None)
    print(f"Finalized manifest: {args.manifest}")
    return 0


def _cmd_validate(args):
    m = read(args.manifest)
    errs = validate_manifest(m)
    if errs:
        print("Manifest validation failed:")
        for e in errs:
            print(f" - {e}")
        return 2
    print("Manifest is valid.")
    return 0


def main(argv=None) -> int:
    import argparse
    p = argparse.ArgumentParser(prog="pylabhub-manifest", description="pyLabHub manifest helper")
    sub = p.add_subparsers(dest="cmd", required=True)

    p_init = sub.add_parser("init", help="create a new manifest in a run directory")
    p_init.add_argument("--run-dir", required=True)
    p_init.add_argument("--run-id", required=True)
    p_init.add_argument("--description", default="")
    p_init.add_argument("--manifest", default="manifest.yaml")
    p_init.set_defaults(func=_cmd_init)

    p_fin = sub.add_parser("finalize", help="compute stream hashes and self-hash")
    p_fin.add_argument("--manifest", required=True)
    p_fin.add_argument("--run-dir", default=None)
    p_fin.set_defaults(func=_cmd_finalize)

    p_val = sub.add_parser("validate", help="validate manifest structure")
    p_val.add_argument("--manifest", required=True)
    p_val.set_defaults(func=_cmd_validate)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
