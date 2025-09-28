"""
pyLabHub Hub API — Core Function Set (CFS), aligned to design.md

Channels: control (high priority), events (context/logs/state), data (high throughput)
Deterministic command→ack via msg_id. Bounded queues (blocking on full).
Internal services receive all messages synchronously from the Hub.
Optional shared-memory hot path remains stubbed for CFS.

This is a skeleton for iteration; networking/IPC and persistence backends come later.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, Optional, Tuple, Protocol, Iterable, Mapping
import asyncio
import time
import uuid

# -------------------------
# Core data structures
# -------------------------

@dataclass
class Message:
    """Generic message envelope transported on control/events/data channels.

    header SHOULD include: msg_id, kind, channel, schema, content-type, ts, trace_id
    'ts' contains both wall-clock and monotonic timestamps (see _stamp()).
    buffer is optional for binary payloads (data channel).
    """
    header: Dict[str, Any]
    buffer: Optional[memoryview] = None

@dataclass
class Ack:
    msg_id: str
    ok: bool
    code: str = "ok"
    message: str = ""
    data: Optional[Dict[str, Any]] = None

@dataclass
class StreamSpec:
    name: str
    dtype: str
    shape: Iterable[int] | None = None
    units: str | None = None
    encoding: str | None = None  # e.g., "application/x-numpy"

# -------------------------
# Adapter / Service Protocols
# -------------------------
class Adapter(Protocol):
    id: str
    async def configure(self, **params) -> Ack: ...
    async def start(self) -> Ack: ...
    async def stop(self) -> Ack: ...

class Service(Protocol):
    id: str
    async def on_message(self, msg: Message) -> None: ...

# -------------------------
# Shared Memory Handles
# -------------------------
@dataclass
class ShmHandle:
    name: str
    slots: int
    slot_bytes: int

# -------------------------
# Hub API
# -------------------------
class Hub:
    def __init__(self, hub_id: str,
                 control_maxsize: int = 128,
                 events_maxsize: int = 512,
                 data_maxsize: int = 1024):
        self.hub_id = hub_id
        self._session_token: Optional[str] = None

        # Per-channel bounded queues (CFS in-process transport)
        self._control_q: Optional[asyncio.Queue[Message]] = None
        self._events_q: Optional[asyncio.Queue[Message]] = None
        self._data_q: Optional[asyncio.Queue[Message]] = None
        self._sizes = (control_maxsize, events_maxsize, data_maxsize)

        # Registry
        self._streams: Dict[str, StreamSpec] = {}
        self._adapters: Dict[str, Adapter] = {}
        self._services: Dict[str, Service] = {}

        # Ack correlation by msg_id
        self._ack_waiters: Dict[str, asyncio.Future[Ack]] = {}

    # ---- Session & identity ----
    async def open_session(self, token: str) -> Ack:
        if self._session_token is not None:
            return Ack(msg_id="open_session", ok=False, code="session-already-open")
        self._session_token = token
        c, e, d = self._sizes
        self._control_q = asyncio.Queue(maxsize=c)
        self._events_q  = asyncio.Queue(maxsize=e)
        self._data_q    = asyncio.Queue(maxsize=d)
        return Ack(msg_id="open_session", ok=True)

    async def close_session(self) -> Ack:
        self._session_token = None
        self._control_q = None
        self._events_q = None
        self._data_q = None
        self._streams.clear()
        return Ack(msg_id="close_session", ok=True)

    @property
    def session_token(self) -> Optional[str]:
        return self._session_token

    # ---- Registration ----
    def register_adapter(self, adapter: Adapter) -> None:
        self._adapters[adapter.id] = adapter

    def register_service(self, service: Service) -> None:
        self._services[service.id] = service

    # ---- Helpers ----
    def _require_session(self) -> None:
        if not (self._control_q and self._events_q and self._data_q):
            raise RuntimeError("no-session")

    def _stamp(self, hdr: Mapping[str, Any]) -> Dict[str, Any]:
        """Ensure msg_id and timestamps exist; attach hub_id if absent."""
        msg_id = hdr.get("msg_id") or str(uuid.uuid4())
        # Wall-clock in ISO 8601 + monotonic seconds
        wall = time.time()
        mono = time.monotonic()
        ts = hdr.get("ts") or {"wall_clock": wall, "monotonic": mono}
        out = dict(hdr)
        out.setdefault("msg_id", msg_id)
        out.setdefault("ts", ts)
        out.setdefault("trace_id", hdr.get("trace_id") or msg_id)
        out.setdefault("hub_id", self.hub_id)
        return out

    async def _dispatch_to_services(self, msg: Message) -> None:
        if not self._services:
            return
        await asyncio.gather(*(svc.on_message(msg) for svc in self._services.values()),
                             return_exceptions=True)

    # ---- Ack correlation API ----
    async def await_ack(self, msg_id: str, timeout: float | None = None) -> Ack:
        fut = self._ack_waiters.get(msg_id)
        if fut is None:
            # No waiter was registered (e.g., nothing generates acks in CFS); return optimistic OK
            return Ack(msg_id=msg_id, ok=True)
        return await asyncio.wait_for(fut, timeout=timeout)

    def post_ack(self, ack: Ack) -> None:
        """Resolve a pending ack waiter by msg_id (to be called by a control handler)."""
        fut = self._ack_waiters.pop(ack.msg_id, None)
        if fut and not fut.done():
            fut.set_result(ack)

    # ---- Control channel ----
    async def send_control(self, header: Mapping[str, Any], payload: Mapping[str, Any] | None = None,
                           await_timeout: float | None = None) -> Ack:
        """Send a control command and await deterministic ack by msg_id."""
        self._require_session()
        hdr = {"kind": "command", "channel": "control",
               "schema": header.get("schema") or "pylabhub/command@1",
               "content-type": header.get("content-type") or "application/json"}
        hdr.update(header)
        hdr = self._stamp(hdr)
        msg = Message(header=hdr, buffer=None)
        # register waiter
        fut: asyncio.Future[Ack] = asyncio.get_running_loop().create_future()
        self._ack_waiters[msg.header["msg_id"]] = fut
        await self._control_q.put(msg)
        await self._dispatch_to_services(msg)
        # If no real handler posts an ack, await_ack returns optimistic OK.
        return await self.await_ack(msg.header["msg_id"], timeout=await_timeout)

    # ---- Events & state ----
    async def emit_event(self, name: str, payload: Mapping[str, Any],
                         schema: str = "pylabhub/events.log@1") -> Ack:
        self._require_session()
        hdr = self._stamp({
            "kind": "event",
            "channel": "events",
            "schema": schema,
            "content-type": "application/json",
            "event": name,
        })
        msg = Message(header=hdr, buffer=None)
        await self._events_q.put(msg)
        await self._dispatch_to_services(msg)
        return Ack(msg_id=hdr["msg_id"], ok=True)

    async def emit_state(self, scope: str, fields: Mapping[str, Any],
                         schema: str = "pylabhub/state.instrument@1") -> Ack:
        self._require_session()
        hdr = self._stamp({
            "kind": "state",
            "channel": "events",
            "schema": schema,
            "content-type": "application/json",
            "scope": scope,
        })
        msg = Message(header=hdr, buffer=None)
        await self._events_q.put(msg)
        await self._dispatch_to_services(msg)
        return Ack(msg_id=hdr["msg_id"], ok=True)

    # ---- Data channel ----
    async def create_stream(self, name: str, spec: StreamSpec) -> Ack:
        self._streams[name] = spec
        return Ack(msg_id=f"create_stream:{name}", ok=True)

    async def send_data(self, stream: str, header: Mapping[str, Any],
                        buffer: bytes | memoryview) -> Ack:
        self._require_session()
        if stream not in self._streams:
            return Ack(msg_id=str(header.get("msg_id") or ""), ok=False, code="unknown-stream",
                       message=f"Stream '{stream}' not created")
        hdr = {"kind": "data", "channel": "data", "stream": stream,
               "schema": header.get("schema") or "pylabhub/stream.append@1",
               "content-type": header.get("content-type") or (self._streams[stream].encoding or "application/octet-stream")}
        hdr.update(header)
        hdr = self._stamp(hdr)
        mv = memoryview(buffer) if not isinstance(buffer, memoryview) else buffer
        msg = Message(header=hdr, buffer=mv)
        await self._data_q.put(msg)  # blocks if full -> prevents loss
        await self._dispatch_to_services(msg)
        return Ack(msg_id=hdr["msg_id"], ok=True)

    async def append(self, stream: str, shard: bytes | memoryview) -> Ack:
        return await self.send_data(stream, header={}, buffer=shard)

    # ---- Pull API (priority: control -> events -> data) ----
    async def recv_next(self) -> Message:
        """Pop next message with control preemption, then events, then data."""
        self._require_session()
        try:
            return self._control_q.get_nowait()  # type: ignore[return-value]
        except asyncio.QueueEmpty:
            pass
        try:
            return self._events_q.get_nowait()  # type: ignore[return-value]
        except asyncio.QueueEmpty:
            pass
        return await self._data_q.get()  # type: ignore[return-value]

    # ---- Timing & health ----
    async def get_timebase(self) -> Dict[str, Any]:
        return {"wall_clock": time.time(), "monotonic": time.monotonic(), "device_offsets": {}}

    async def get_health(self) -> Dict[str, Any]:
        return {"hub_id": self.hub_id, "status": "ok",
                "queues": {"control": self._control_q.qsize() if self._control_q else None,
                           "events": self._events_q.qsize() if self._events_q else None,
                           "data": self._data_q.qsize() if self._data_q else None}}

    # ---- Shared memory (optional hot path; stubbed) ----
    async def shm_create(self, stream: str, slots: int, slot_bytes: int) -> ShmHandle:
        return ShmHandle(name=f"/pylabhub/{self.hub_id}/{stream}", slots=slots, slot_bytes=slot_bytes)

    async def shm_write(self, handle: ShmHandle, hdr: Mapping[str, Any], payload_ptr: Any) -> Ack:
        return Ack(msg_id=f"shm_write:{handle.name}", ok=True)

    async def shm_read(self, handle: ShmHandle) -> Tuple[Dict[str, Any], memoryview]:
        return {}, memoryview(b"")

    async def shm_close(self, handle: ShmHandle) -> Ack:
        return Ack(msg_id=f"shm_close:{handle.name}", ok=True)
