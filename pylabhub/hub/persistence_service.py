from __future__ import annotations
import asyncio
import json
import os
from typing import Any, Dict, Optional

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import zarr

# Import the Hub types
# from api import Message, Service, Ack, Hub, StreamSpec  # <- use your actual module path

class PersistenceService:
    """Writes events/state to Parquet; data chunks to Zarr per stream."""
    id = "persistence"

    def __init__(self,
                 base_dir: str,
                 events_filename: str = "events.parquet",
                 zarr_dirname: str = "zarr"):
        self.base_dir = base_dir
        self.events_path = os.path.join(base_dir, events_filename)
        self.zarr_root = os.path.join(base_dir, zarr_dirname)

        os.makedirs(self.base_dir, exist_ok=True)
        os.makedirs(self.zarr_root, exist_ok=True)

        self._parquet_writer: Optional[pq.ParquetWriter] = None
        self._parquet_schema: Optional[pa.schema] = None

        # cache of zarr arrays keyed by stream name
        self._zarr_root = zarr.open(self.zarr_root, mode="a")
        self._zarr_arrays: dict[str, zarr.Array] = {}

        # simple lock so writes are consistent even if on_message is concurrent
        self._lock = asyncio.Lock()

    async def on_message(self, msg) -> None:
        # NOTE: the Hub calls this for every message on any channel.
        channel = msg.header.get("channel")
        kind = msg.header.get("kind")

        # Persist events + state to Parquet
        if channel == "events" and kind in ("event", "state"):
            await self._write_event_row(msg)

        # Persist data to Zarr
        if channel == "data" and kind == "data":
            await self._append_data_chunk(msg)

    # -------------------------
    # Parquet (events & state)
    # -------------------------
    async def _write_event_row(self, msg) -> None:
        # Build a flat dict row from header; payload lives in header fields.
        # If the app wants to put a JSON-able payload under 'payload', do so in the header.
        row = {
            "hub_id": msg.header.get("hub_id"),
            "msg_id": msg.header.get("msg_id"),
            "trace_id": msg.header.get("trace_id"),
            "kind": msg.header.get("kind"),
            "event": msg.header.get("event"),
            "schema": msg.header.get("schema"),
            "content_type": msg.header.get("content-type"),
            "channel": msg.header.get("channel"),
            "scope": msg.header.get("scope"),
            "ts_wall": (msg.header.get("ts") or {}).get("wall_clock"),
            "ts_mono": (msg.header.get("ts") or {}).get("monotonic"),
            # Optional structured data payloads (must be JSON-serializable)
            "payload_json": json.dumps(msg.header.get("payload", {}), separators=(",", ":"), ensure_ascii=False),
        }

        # Convert to Arrow
        table = pa.Table.from_pylist([row])

        async with self._lock:
            if self._parquet_writer is None:
                self._parquet_schema = table.schema
                self._parquet_writer = pq.ParquetWriter(self.events_path, self._parquet_schema)
            else:
                # Schema evolution: align columns order; add missing as nulls
                table = self._align_schema(table, self._parquet_schema)

            self._parquet_writer.write_table(table)

    def _align_schema(self, table: pa.Table, schema: pa.Schema) -> pa.Table:
        # Reorder/extend columns to match initial schema; unknown columns are dropped.
        cols = []
        for field in schema:
            name = field.name
            if name in table.column_names:
                col = table[name]
            else:
                col = pa.array([None] * table.num_rows, type=field.type)
            cols.append(col)
        return pa.Table.from_arrays(cols, schema.names)

    # -------------------------
    # Zarr (data channel)
    # -------------------------
    async def _append_data_chunk(self, msg) -> None:
        stream = msg.header.get("stream")
        if not stream:
            return

        # Expect dtype & shape in header for decoding the buffer
        dtype_str = msg.header.get("dtype")
        shape = msg.header.get("shape")
        if not dtype_str or not shape:
            # Can't decode reliably; skip
            return

        # decode the buffer into an array with the given dtype/shape
        buf = msg.buffer
        if buf is None:
            return

        arr = np.frombuffer(buf, dtype=np.dtype(dtype_str)).reshape(shape)

        async with self._lock:
            za = self._zarr_arrays.get(stream)
            if za is None:
                # Create an appendable array with first dimension = samples, remainder = shape
                # We'll treat each message as one "sample" along axis=0.
                sample_shape = tuple(shape)
                total_shape = (0,) + sample_shape   # growable along axis 0
                chunks = (1,) + sample_shape        # one sample per chunk by default; tune if needed

                za = self._zarr_root.require_dataset(
                    name=stream,
                    shape=total_shape,
                    chunks=chunks,
                    dtype=arr.dtype,
                    fill_value=None,
                    overwrite=False,
                    dimension_separator="/"
                )
                self._zarr_arrays[stream] = za

            # Append along axis 0
            new_len = za.shape[0] + 1
            za.resize((new_len, *za.shape[1:]))
            za[new_len - 1, ...] = arr

    # -------------------------
    # Cleanup
    # -------------------------
    async def close(self) -> None:
        async with self._lock:
            if self._parquet_writer is not None:
                self._parquet_writer.close()
                self._parquet_writer = None
