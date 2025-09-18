import json
from pathlib import Path

from pylabhub.hub import manifest as mf


def test_init_finalize_validate(tmp_path):
    # Arrange: set up a fake run directory with a dummy Zarr-like tree and an events file
    run_id = "run-0001"
    run_dir = tmp_path / "runs" / run_id
    (run_dir / "arrays" / "raw_adc.zarr").mkdir(parents=True, exist_ok=True)
    (run_dir / "arrays" / "raw_adc.zarr" / "chunk_0").write_bytes(b"abc")

    (run_dir / "events").mkdir(parents=True, exist_ok=True)
    # Note: we don't need a real Parquet file for hashing; this is a structural test.
    (run_dir / "events" / "control-0001.parquet").write_text("not-a-real-parquet")

    manifest_path = run_dir / "manifest.yaml"

    # Create a minimal manifest
    m = mf.init_manifest(run_id=run_id, description="test run")
    m["acquisition"]["streams"] = [
        {
            "name": "raw_adc",
            "kind": "array",
            "format": "zarr",
            "path": "arrays/raw_adc.zarr",
        },
        {
            "name": "control",
            "kind": "events",
            "format": "parquet",
            "path": "events/control-0001.parquet",
        },
    ]

    # Act: write, finalize (hashes + sizes + self-hash), then validate
    mf.write(m, manifest_path)
    mf.finalize_manifest(manifest_path, run_dir=run_dir)
    m2 = mf.read(manifest_path)
    errors = mf.validate_manifest(m2)

    # Assert
    assert errors == []
    assert m2["integrity"]["manifest_sha256"], "self-hash should be set"
    # Stream hashes should be present
    hashes = [s.get("sha256") for s in m2["acquisition"]["streams"]]
    assert all(hashes), "each stream should have a sha256 after finalize"


def test_self_hash_changes_when_stream_changes(tmp_path):
    run_id = "run-0002"
    run_dir = tmp_path / "runs" / run_id
    zarr_dir = run_dir / "arrays" / "raw_adc.zarr"
    zarr_dir.mkdir(parents=True, exist_ok=True)
    chunk = zarr_dir / "chunk_0"
    chunk.write_bytes(b"abc")

    (run_dir / "events").mkdir(parents=True, exist_ok=True)
    (run_dir / "events" / "control-0002.parquet").write_text("not-a-real-parquet")

    manifest_path = run_dir / "manifest.yaml"

    m = mf.init_manifest(run_id=run_id, description="hash change test")
    m["acquisition"]["streams"] = [
        {"name": "raw_adc", "kind": "array", "format": "zarr", "path": "arrays/raw_adc.zarr"},
        {"name": "control", "kind": "events", "format": "parquet", "path": "events/control-0002.parquet"},
    ]

    mf.write(m, manifest_path)
    mf.finalize_manifest(manifest_path, run_dir=run_dir)
    before = mf.read(manifest_path)
    before_hash = before["integrity"]["manifest_sha256"]

    # Modify a chunk, finalize again, hash should change
    chunk.write_bytes(b"abcd")
    mf.finalize_manifest(manifest_path, run_dir=run_dir)
    after = mf.read(manifest_path)
    after_hash = after["integrity"]["manifest_sha256"]

    assert before_hash != after_hash, "manifest self-hash should change when stream content changes"
