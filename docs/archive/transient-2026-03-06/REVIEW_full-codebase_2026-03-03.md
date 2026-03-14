# Code Review: Full C++ Codebase

**Reviewer**: Claude (automated deep review)
**Date**: 2026-03-03
**Branch**: `claude/review-cpp-utils-jobvb`
**Scope**: All 140 source files (~42,000 lines) under `cpp/src/` — utils, actor, hub_python, scripting, and all public headers under `cpp/src/include/`

---

## Summary

This review covers the entire C++ source tree: 42 `.cpp` files + 56 headers in `src/utils/` and `src/include/utils/`, 14 files in `src/actor/`, 10 files in `src/hub_python/`, 2 files in `src/scripting/`, and 5 umbrella headers under `src/include/`. Additionally, HEP design specification documents were cross-referenced against the implementation for consistency.

All 585 tests pass. The codebase compiles cleanly under GCC with `-Wall -Wextra -Werror`.

**Findings**: 15 CRITICAL, 28 HIGH, 47 MEDIUM, 35 LOW, 4 SECURITY — **153 total**

---

## Table of Contents

1. [DataBlock / Shared Memory Subsystem](#1-datablock--shared-memory-subsystem)
2. [IPC / Messenger / Broker / Hub Subsystem](#2-ipc--messenger--broker--hub-subsystem)
3. [Service / Logging / Config / Crypto Subsystem](#3-service--logging--config--crypto-subsystem)
4. [Actor / hub_python / Scripting Subsystem](#4-actor--hub_python--scripting-subsystem)
5. [Public Headers / ABI / API Design](#5-public-headers--abi--api-design)
6. [HEP Specification vs Implementation Consistency](#6-hep-specification-vs-implementation-consistency)
7. [Summary Tables](#7-summary-tables)

---

## 1. DataBlock / Shared Memory Subsystem

**Files reviewed**: `src/utils/shm/data_block.cpp`, `data_block_slot_ops.cpp`, `data_block_mutex.cpp`, `data_block_recovery.cpp`, `shared_memory_spinlock.cpp`, `data_block_internal.hpp` + all corresponding public headers under `src/include/utils/`

### CRITICAL

#### SHM-C1. Heartbeat registration corrupts other consumers' identity data

**File**: `data_block.cpp:2254-2272`
**Severity**: CRITICAL — **data corruption in shared memory**

In `register_heartbeat()`, the consumer writes `consumer_uid` and `consumer_name` into `consumer_heartbeats[i]` *before* the CAS on `consumer_pid` succeeds. If the CAS fails (another consumer owns the slot), the code then zeroes the identity fields it just wrote — but those fields belong to the other consumer.

```cpp
// Writes to a slot BEFORE knowing if we own it:
std::memcpy(header->consumer_heartbeats[i].consumer_uid, pImpl->consumer_uid_buf, ...);
std::memcpy(header->consumer_heartbeats[i].consumer_name, pImpl->consumer_name_buf, ...);
if (header->consumer_heartbeats[i].consumer_pid.compare_exchange_strong(expected, pid, ...))
{
    // We own it -- great
}
// CAS failed -- we just corrupted someone else's uid/name
std::memset(header->consumer_heartbeats[i].consumer_uid, 0, ...);  // Stomps real owner's data!
std::memset(header->consumer_heartbeats[i].consumer_name, 0, ...);
```

**Fix**: Only write identity AFTER the CAS succeeds. Move the `memcpy` calls into the success branch of `compare_exchange_strong`. Remove the cleanup `memset` in the failure branch.

---

#### SHM-C2. `acquire_write_slot` increments `write_index` before lock — creates unrecoverable gaps

**File**: `data_block.cpp:1481-1497`
**Severity**: CRITICAL — **potential consumer deadlock**

The writer atomically increments `write_index` (line 1481) to claim a slot ID, then attempts `acquire_write` on the resulting slot. If `acquire_write` times out (line 1493), the function returns nullptr without decrementing `write_index`. This burns a slot ID permanently. For `Sequential` and `Sequential_sync` policies, the unconsumed slot creates a gap that consumers will spin on forever (they wait for slot N to be committed, but it never will be). With enough timeouts, `write_index` can advance past `read_index` by more than `slot_count`, creating deadlock.

**Fix**: Either (a) mark the timed-out slot as skipped so consumers can advance past it, or (b) do not increment `write_index` until after the lock is acquired (use a separate CAS loop). At minimum, store a sentinel generation or state so consumers do not spin forever.

---

### HIGH

#### SHM-H1. `SharedSpinLock::try_lock_for(0)` means "spin indefinitely"

**File**: `shared_memory_spinlock.cpp:41-106`
**Severity**: HIGH — **contradicts POSIX convention and internal codebase convention**

`try_lock_for(int timeout_ms)` treats `timeout_ms == 0` as "indefinite wait" (line 88: `if (timeout_ms > 0)`). But in `data_block_internal.hpp:323`, `spin_elapsed_ms_exceeded` treats `timeout_ms == 0` as "non-blocking, expire immediately." `SharedSpinLock::lock()` calls `try_lock_for(0)` which is correct for its purpose, but any caller expecting the POSIX convention (`0 = non-blocking`) will get infinite spin.

**Fix**: Make `try_lock_for(0)` mean non-blocking (try once, return false). Have `lock()` call an internal infinite-spin variant.

---

#### SHM-H2. Relaxed ordering on `owner_tid` clear during unlock

**File**: `shared_memory_spinlock.cpp:149-152`
**Severity**: HIGH — **potential stale recursion match with thread pool TID reuse**

The unlock sequence stores `owner_tid = 0` with `relaxed` ordering before `owner_pid = 0` with `release`. In a thread pool where TIDs can be reused, another thread in the same process could see `owner_pid == my_pid` with stale `owner_tid == my_old_tid`, potentially triggering a false recursion match.

**Fix**: Use `release` ordering for `owner_tid.store(0)`. The performance difference is negligible.

---

#### SHM-H3. `const_cast` in `is_producer_heartbeat_fresh`

**File**: `data_block_internal.hpp:257`
**Severity**: HIGH — **technically UB if header is in read-only memory**

Takes `const SharedMemoryHeader*` but `const_cast`s it away to call `producer_heartbeat_id_ptr`. While shared memory is always R/W in practice, this is bad practice.

**Fix**: Add `const` overloads of `producer_heartbeat_id_ptr` and `producer_heartbeat_ns_ptr`.

---

#### SHM-H4. `datablock_diagnose_all_slots` re-opens SHM N times

**File**: `data_block_recovery.cpp:169-173`
**Severity**: HIGH — **performance + TOCTOU**

Each call to `datablock_diagnose_slot` calls `open_for_recovery` which attaches/detaches the SHM segment. For N slots, this opens and closes SHM N times.

**Fix**: Extract `open_for_recovery` outside the loop and pass the context directly.

---

#### SHM-H5. `reader_count` underflow not detected in `release_read`

**File**: `data_block_slot_ops.cpp:233`
**Severity**: HIGH — **can permanently block all writers**

`release_read` does `reader_count.fetch_sub(1)` unconditionally. A double-release wraps `reader_count` to `UINT32_MAX`, permanently blocking all writers waiting for `reader_count == 0`.

**Fix**: Capture the return value of `fetch_sub`; if it was already 0 (wrapped), log an error and store 0 back.

---

#### SHM-H6. ABI violation: `std::string` in exported classes without pImpl

**Files**: `integrity_validator.hpp:43`, `slot_diagnostics.hpp:56-57`, `data_block_mutex.hpp:86`
**Severity**: HIGH — **ABI break across MSVC versions**

`IntegrityValidator`, `SlotDiagnostics`, and `DataBlockMutex` are `PYLABHUB_UTILS_EXPORT` but have `std::string` private members. CLAUDE.md states "Pimpl idiom is mandatory for all public classes in pylabhub-utils."

**Fix**: Either use pImpl or replace `std::string` members with fixed-size `char[]` arrays (as done for `SharedSpinLock::m_name`).

---

### MEDIUM

#### SHM-M1. Spinlock allocation race window after CAS + init

**File**: `data_block.cpp:660-679`

`acquire_shared_spinlock` CASes `owner_pid` from 0→1, then `init_spinlock_state` stores 0 back. Between the CAS and init, another process could see `owner_pid == 1` and attempt reclaim. After init stores 0, another call could re-claim the same slot.

**Fix**: Use a separate allocation flag or sentinel value that `init_spinlock_state` does not clear.

---

#### SHM-M2. Null `src` not checked in `SlotWriteHandle::write()`

**File**: `data_block.cpp:1867`

A null `src` with `len > 0` causes `memcpy` UB.

**Fix**: Add `if (src == nullptr) return false;`.

---

#### SHM-M3. Integer overflow in bounds check — `write()` and `read()`

**Files**: `data_block.cpp:1863`, `data_block.cpp:1956`

`offset + len > buffer_size` can overflow if both values are large.

**Fix**: `if (len > buffer_size || offset > buffer_size - len)`.

---

#### SHM-M4. `DataBlockMutex` uses `CLOCK_REALTIME` — subject to system clock jumps

**File**: `data_block_mutex.cpp:367`

`try_lock_for` uses `clock_gettime(CLOCK_REALTIME)` with `pthread_mutex_timedlock`. NTP corrections can extend or shorten the timeout. The rest of the codebase uses `monotonic_time_ns()`.

**Fix**: Use `CLOCK_MONOTONIC` with `pthread_mutexattr_setclock` during mutex creation.

---

#### SHM-M5. Missing `PYLABHUB_UTILS_EXPORT` on some `extern "C"` recovery functions

**File**: `data_block_recovery.cpp:811-868`

`datablock_get_metrics`, `datablock_reset_metrics`, `datablock_get_channel_identity`, `datablock_list_consumers` lack the export macro in their definitions. With `-fvisibility=hidden`, these symbols won't be exported.

**Fix**: Add `PYLABHUB_UTILS_EXPORT` to the definitions.

---

#### SHM-M6. `from_header` trusts `flexible_zone_size` alignment without validation

**File**: `data_block.cpp:180`

If the header is corrupted (bit flip in SHM), a misaligned `flexible_zone_size` causes `std::logic_error` during attach rather than a recoverable error.

**Fix**: Validate alignment and return an error or default layout.

---

#### SHM-M7. `slot_rw_coordinator.h` is not a pure C header

**File**: `slot_rw_coordinator.h:26-30`

Contains `namespace pylabhub::hub { ... }` before the `extern "C"` block. A C compiler will error on the `namespace` keyword.

**Fix**: Either rename to `.hpp`, or wrap namespace declarations in `#ifdef __cplusplus` guards.

---

#### SHM-M8. SHM handle leak on diagnostic magic validation failure

**File**: `data_block.cpp:1239-1243`

If magic validation fails, the function returns nullptr without calling `shm_close()`. The `impl` unique_ptr destructor doesn't close the SHM handle.

**Fix**: Call `shm_close()` before returning, or add a destructor to `DataBlockDiagnosticHandleImpl`.

---

### LOW

#### SHM-L1. Missing `[[nodiscard]]` on spinlock/mutex methods

**Files**: `shared_memory_spinlock.hpp:97,113`, `data_block_mutex.hpp:78`, `slot_diagnostics.hpp:39-53`

#### SHM-L2. `noexcept` functions that allocate `std::string`

**File**: `data_block.cpp:1338-1370` — `hub_uid()` etc. are `noexcept` but internally construct `std::string` which can throw `std::bad_alloc`. Documented design choice (OOM → terminate).

#### SHM-L3. Layout validation only compiled in debug builds

**File**: `data_block.cpp:221-274` — `validate()` is `#if !defined(NDEBUG)` only. Critical for catching SHM corruption in production.

#### SHM-L4. `reinterpret_cast` to `std::atomic<uint8_t>*` on non-atomic memory

**File**: `data_block.cpp:836-843` — Technically UB per the standard, though it works on all major platforms.

#### SHM-L5. `SlotDiagnostic` struct may have uninitialized padding

**File**: `recovery_api.hpp:47-59`

### SECURITY

#### SHM-S1. Non-constant-time shared secret comparison

**File**: `data_block.cpp:2618`

`std::memcmp` short-circuits, enabling timing side-channel attacks. Use `sodium_memcmp` (already a dependency).

---

#### SHM-S2. Only 8 bytes of 64-byte secret actually validated

**File**: `data_block.cpp:393-399`

The `shared_secret` field is 64 bytes but the comparison (`sizeof(uint64_t)`) only checks the first 8. Effective entropy is 64 bits.

---

## 2. IPC / Messenger / Broker / Hub Subsystem

**Files reviewed**: `src/utils/ipc/messenger.cpp`, `broker_service.cpp`, `zmq_context.cpp`, `messenger_protocol.cpp`, `messenger_internal.hpp`, `channel_registry.cpp`, `channel_handle.cpp`, `heartbeat_manager.cpp` + `src/utils/hub/hub_producer.cpp`, `hub_consumer.cpp` + all corresponding headers

### CRITICAL

#### IPC-C1. CurveZMQ secret key material never zeroed

**Files**: `messenger.cpp:296-313,528-536,637-643`, `broker_service.cpp:885-892`, `messenger_internal.hpp:252-253`
**Severity**: CRITICAL — **cryptographic key exposure**

Secret keys from `zmq_curve_keypair()` are stored in stack-local `char` arrays and `std::string` members. When they go out of scope or are destroyed, the key material remains in memory. The project includes `sodium.h` but never calls `sodium_memzero()` on any secret key buffer. An attacker with a memory read primitive (core dump, swap file, `/proc/pid/mem`) can recover all CurveZMQ secret keys.

**Fix**: Call `sodium_memzero()` after each `zmq_curve_keypair()` once the key is set on the socket. Clear `m_client_secret_key_z85` in `DisconnectCmd`/`StopCmd` handlers and in `~MessengerImpl()`. Consider a RAII type that zeros in its destructor.

---

#### IPC-C2. Race condition in `zmq_context_startup()` — double allocation

**File**: `zmq_context.cpp:37-58`
**Severity**: CRITICAL — **resource leak, potential use-after-free**

Non-atomic check-then-act: loads `g_context` with `acquire`, checks for `nullptr`, creates a new context, and stores it. Two concurrent callers can both see `nullptr`, both allocate, and one is leaked.

**Fix**: Use `std::call_once`:
```cpp
void zmq_context_startup() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        auto *ctx = new zmq::context_t(1);
        zmq_ctx_set(ctx->handle(), ZMQ_BLOCKY, 0);
        g_context.store(ctx, std::memory_order_release);
    });
}
```

---

#### IPC-C3. Producer/Consumer `start()` thread lambdas capture `this` — use-after-move

**Files**: `hub_producer.cpp:569,573`, `hub_consumer.cpp:536,540,545`
**Severity**: CRITICAL — **dangling pointer in running thread**

Thread lambdas capture `this` (the `Producer*`/`Consumer*`). If the object is moved after `start()`, `this` becomes dangling. The move constructor is defaulted with no guard.

**Fix**: Capture `pImpl.get()` (heap-stable pointer) instead of `this`:
```cpp
pImpl->peer_thread_handle = std::thread([p = pImpl.get()] { p->run_peer_thread(); });
```

---

### HIGH

#### IPC-H1. `assert()` used for runtime invariants — silent in release

**Files**: `zmq_context.cpp:32`, `messenger.cpp:765-769`, `hub_producer.cpp:728,763,813`, `hub_consumer.cpp:700,726,763`, `channel_handle.cpp:129`

These guard against incorrect lifecycle ordering (a runtime/configuration issue), not pure programming invariants. In release builds with `NDEBUG`, nullptr dereference occurs silently.

**Fix**: Replace with runtime checks that throw `std::logic_error` or call `PLH_PANIC`.

---

#### IPC-H2. BrokerService Config stores unzeroed `std::string` secret key

**Files**: `broker_service.hpp:90`, `broker_service.cpp:879`

`server_secret_key` is moved into pImpl but never zeroed on destruction.

**Fix**: Add a destructor to `BrokerServiceImpl` calling `sodium_memzero` on secret key fields.

---

#### IPC-H3. Data race on Producer/Consumer callback members

**Files**: `hub_producer.cpp:69-74,504-550`, `hub_consumer.cpp` (similar)

Callback `std::function` objects are set from caller's thread but read from `peer_thread` / `data_thread` with no synchronization. This is UB.

**Fix**: Either enforce "set before `start()`" with a runtime check, or protect callbacks with a mutex.

---

#### IPC-H4. `Messenger::connect()` / `create_channel()` — `future.get()` with no timeout

**Files**: `messenger.cpp:449-453,461-464,567-586,613-624`

If the worker thread is blocked or deadlocked, the calling thread hangs indefinitely.

**Fix**: Use `future.wait_for()` with a reasonable timeout.

---

#### IPC-H5. `zmq_context_shutdown()` — delete-then-store-nullptr race window

**File**: `zmq_context.cpp:61-76`

Between `delete ctx` (line 74) and `g_context.store(nullptr)` (line 75), concurrent readers can load the deleted pointer.

**Fix**: Use `exchange` to atomically replace before deleting:
```cpp
auto *ctx = g_context.exchange(nullptr, std::memory_order_acq_rel);
if (!ctx) return;
ctx->shutdown();
delete ctx;
```

---

#### IPC-H6. `DiscoverProducerCmd` retry loop blocks entire worker thread

**File**: `messenger_protocol.cpp:377-468`

The discovery loop uses `sleep_for()` and repeated `send_disc_req()`. While running, no other commands are processed: no heartbeats, no notifications, no new connect/disconnect requests. Can cause heartbeat timeout on the broker side.

**Fix**: Break discovery into a state machine that yields back to the worker loop between retries.

---

#### IPC-H7. `ManagedProducer`/`ManagedConsumer` registry — dangling pointer after move

**Files**: `hub_producer.cpp:860-896`, `hub_consumer.cpp:810-850`

`get_module_def()` stores `this` in `g_producer_registry`. The class has a defaulted move constructor. Moving after `get_module_def()` leaves a dangling pointer.

**Fix**: Delete move operations, or update the registry entry in move operations.

---

### MEDIUM

#### IPC-M1. Missing `[[nodiscard]]` on `send()` / `recv()` methods

**Files**: `channel_handle.hpp:62,96,107`, `hub_producer.hpp:342-343`

#### IPC-M2. `ChannelRegistry::find_channel()` returns a copy — TOCTOU with deregister

**File**: `broker_service.cpp:675-688`

The pattern is safe due to single-threaded run() loop but is fragile.

#### IPC-M3. Header comment contradicts implementation in `find_timed_out_channels`

**File**: `channel_registry.hpp:144-146` vs `channel_registry.cpp:114-130`

Comment says "Only channels in Ready status," but code times out ALL channels.

**Fix**: Update comment or add the status filter.

#### IPC-M4. Partial multipart sends on exception

**Files**: `broker_service.cpp:821-831,837-849`

4 separate `socket.send()` calls with `sndmore`. If any throws, the ROUTER socket has a partial multipart frame, corrupting subsequent sends.

**Fix**: Use `zmq::send_multipart()`.

#### IPC-M5. No validation that heartbeat `producer_pid` matches registrant

**File**: `broker_service.cpp:576`

A malicious client could send heartbeats for channels it doesn't own.

**Fix**: Validate `producer_pid` against `entry.producer_pid`.

#### IPC-M6. Producer write thread CPU spin loop

**File**: `hub_producer.cpp:296-313`

When a real-time handler returns quickly (e.g., `with_transaction` times out), the write thread loops at 100% CPU.

**Fix**: Add `std::this_thread::yield()` or a small sleep after handler returns.

#### IPC-M7. Broker `on_ready` callback observes partially initialized state

**File**: `broker_service.cpp:186-189`

Called before poll loop starts. Should be documented.

#### IPC-M8. `ConnectChannelCmd` blocks worker via nested `DiscoverProducerCmd`

**File**: `messenger_protocol.cpp:560-662`

Compounds IPC-H6 — nested discovery can block for full `timeout_ms`.

### LOW

#### IPC-L1. `std::string` in exported structs

**Files**: `broker_service.hpp:42-51` (`ChannelSnapshotEntry`), `channel_access_policy.hpp:84-96`, `messenger.hpp:47-81`

#### IPC-L2. Inconsistent error handling — some protocol handlers set promise, some don't

**File**: `messenger_protocol.cpp`

#### IPC-L3. `hex_decode_schema_hash` returns empty on invalid input with no error indication

**File**: `messenger_internal.hpp:96-115` — should return `std::optional`.

#### IPC-L4. `HeartbeatManager` stores raw pointer with no lifetime tracking

**File**: `heartbeat_manager.hpp:58`

#### IPC-L5. Duplicate `static const std::string kEmpty` locals

**Files**: `channel_handle.cpp:89,95`, `hub_producer.cpp:792`, `hub_consumer.cpp:741`

#### IPC-L6. Consumer `close()` sends `'\0'` pointer with size 0

**File**: `hub_consumer.cpp:796-798` — passing `nullptr, 0` would be clearer.

#### IPC-L7. Missing `[[nodiscard]]` on `ChannelHandle` introspection methods

**File**: `channel_handle.hpp:114-117`

---

## 3. Service / Logging / Config / Crypto Subsystem

**Files reviewed**: `src/utils/service/lifecycle.cpp`, `lifecycle_topology.cpp`, `lifecycle_dynamic.cpp`, `lifecycle_impl.hpp`, `file_lock.cpp`, `vault_crypto.cpp`, `hub_vault.cpp`, `actor_vault.cpp`, `crypto_utils.cpp` + `src/utils/logging/logger.cpp` + all sink implementations + `src/utils/config/json_config.cpp`, `hub_config.cpp`, `uid_utils.hpp` + `src/utils/core/debug_info.cpp`, `format_tools.cpp`, `platform.cpp`, `uuid_utils.cpp`

### CRITICAL

#### SVC-C1. Secret key material not zeroed in `vault_crypto.cpp`

**File**: `vault_crypto.cpp:65-84` (vault_derive_key), `vault_crypto.cpp:86-113` (vault_write), `vault_crypto.cpp:115-139` (vault_read)
**Severity**: CRITICAL — **key material persists in process memory**

The derived encryption key (`std::array<uint8_t, kVaultKeyBytes> key`) is never wiped with `sodium_memzero()` after use. Stack copies exist in `vault_write()` and `vault_read()`. These copies remain recoverable from process memory or core dumps.

**Fix**: Add `sodium_memzero(key.data(), key.size())` in a scope guard after use:
```cpp
auto key_guard = basics::make_scope_guard([&key]() {
    sodium_memzero(key.data(), key.size());
});
```

---

#### SVC-C2. Secret key material not zeroed in `hub_vault.cpp` and `actor_vault.cpp`

**Files**: `hub_vault.cpp:81-86,99-101`, `actor_vault.cpp:69-74,95-97`
**Severity**: CRITICAL — **key material persists in heap memory**

Z85-encoded secret keys from `zmq_curve_keypair()` stored in stack-local `std::array<char,41>` are never zeroed after being copied into `std::string` members. The `Impl` struct holding secret key strings is never zeroed on destruction.

**Fix**: (1) Zero the `sec` array after copying: `sodium_memzero(sec.data(), sec.size())`. (2) Add destructors to `Impl` structs that zero secret fields.

---

#### SVC-C3. Secret key material not zeroed in `generate_admin_token`

**File**: `hub_vault.cpp:51-57`
**Severity**: CRITICAL — **admin token material in stack memory**

The `raw` byte array and `hex` char array contain sensitive admin token material and are never zeroed.

**Fix**: Add a scope guard zeroing both arrays.

---

### HIGH

#### SVC-H1. Wrong platform macro in `syslog_sink.hpp`

**File**: `syslog_sink.hpp:8`
**Severity**: HIGH — **Windows build failure**

Uses `PLATFORM_WIN64` instead of the project-standard `PYLABHUB_PLATFORM_WIN64`. The macro is never defined by the project's platform detection, so `SyslogSink` compiles on Windows where syslog is unavailable.

**Fix**: Change to `#if !defined(PYLABHUB_PLATFORM_WIN64)`.

---

#### SVC-H2. WriteLock released before disk write completes

**File**: `json_config.hpp:764`
**Severity**: HIGH — **consistency violation on disk write failure**

In `consume_write_`, after snapshotting JSON data, the in-memory WriteLock is released before calling `atomic_write_json`. Another thread can acquire the write lock and modify in-memory data. If the disk write then fails, in-memory state has already diverged.

**Fix**: Keep the WriteLock held through the disk write.

---

#### SVC-H3. `HubVault::open` does not validate Z85 key lengths

**File**: `hub_vault.cpp:117-132`
**Severity**: HIGH — **corrupted vault → UB in ZMQ**

`ActorVault::open()` validates key lengths at 40 chars, but `HubVault::open()` does not. Corrupted vault data with wrong-length keys causes UB in ZMQ CurveZMQ functions.

**Fix**: Add length validation after parsing in `HubVault::open()`.

---

#### SVC-H4. Race condition on `m_dropping_since` in logger

**File**: `logger.cpp:376-379`
**Severity**: HIGH — **data race (UB)**

`m_dropping_since` is a `time_point` (not atomic). Written by enqueue threads, read by the worker thread without synchronization.

**Fix**: Protect writes inside `queue_mutex_`, or use `std::atomic<int64_t>` for the epoch count.

---

#### SVC-H5. Potential deadlock: `write_sync` holds `m_sink_mutex` while sink may log

**File**: `logger.cpp:1021-1053`
**Severity**: HIGH — **potential deadlock**

`write_sync()` acquires `m_sink_mutex` then calls `sink_->write()`. If the sink calls any Logger method, it re-acquires locks. If `write_sync` is called while the worker is blocked waiting for the calling thread, deadlock occurs.

**Fix**: Document that `write_sync` must not be called when the worker might be waiting. Consider `try_lock` with timeout.

---

### MEDIUM

#### SVC-M1. TOCTOU in `JsonConfig::init` symlink check

**File**: `json_config.cpp:219-229`

Checks `fs::is_symlink()` then opens the file. Between check and open, an attacker could replace the file with a symlink.

**Fix**: Use `O_NOFOLLOW` on POSIX.

---

#### SVC-M2. `HubConfig::load` uses `std::getenv` (not thread-safe)

**File**: `hub_config.cpp:318,367-372`

`std::getenv()` is not thread-safe if another thread calls `setenv()`/`putenv()`.

**Fix**: Copy result immediately (already done implicitly), or use environment access mutex.

---

#### SVC-M3. `waitForUnload` captures `string_view` in lambda

**File**: `lifecycle_dynamic.cpp:719-720`

A `string_view` captured in the predicate lambda dangles if the caller's string is destroyed during the wait.

**Fix**: Convert to `std::string` and capture by value.

---

#### SVC-M4. `InternalGraphNode` has `std::atomic` members but no deleted copy/move

**File**: `lifecycle_impl.hpp:210-233`

`std::atomic` is not copyable/movable. The struct works because nodes are emplaced in-place, but it's fragile.

**Fix**: Explicitly delete copy/move constructors and assignment operators.

---

#### SVC-M5. `validate_module_name` in anonymous namespace of a header included by 3 TUs

**File**: `lifecycle_impl.hpp:31-54`

Each TU gets its own copy of `validate_module_name` and `timedShutdown`, bloating code size.

**Fix**: Move to a `.cpp` file or mark `inline`.

---

#### SVC-M6. `WriteLock::json()` returns reference to `thread_local` dummy on null owner

**File**: `json_config.cpp:651-663`

Writes to the dummy silently disappear. The caller has no way to detect the write was lost.

**Fix**: Add `is_valid()` method or use a sentinel.

---

#### SVC-M7. `ReadLock::json()` returns reference to `static const json` on null owner

**File**: `json_config.cpp:631-639`

Callers silently get an empty JSON with no indication of failure.

---

#### SVC-M8. `hub_vault.cpp` `publish_public_key` does not use atomic write

**File**: `hub_vault.cpp:157-173`

Direct `std::ofstream` write. If the process crashes mid-write, the file is left empty/partial. Other code uses `atomic_write_json` for safe writes.

**Fix**: Write to temporary then atomically rename.

---

### LOW

#### SVC-L1. `uuid_utils.cpp` calls `sodium_init()` on every `generate_uuid4()` call

**File**: `uuid_utils.cpp:22-23` — Unnecessary overhead.

#### SVC-L2. `compute_blake2b_array` double-fills array on failure

**File**: `crypto_utils.cpp:103-114` — Redundant `hash.fill(0)`.

#### SVC-L3. Missing `[[nodiscard]]` on vault/crypto/lifecycle functions

**Files**: `hub_vault.hpp:63,76`, `actor_vault.hpp:69,82`, `crypto_utils.hpp`, `uuid_utils.hpp`

#### SVC-L4. Duplicate `#include "utils/debug_info.hpp"` in `debug_info.cpp`

**File**: `debug_info.cpp:14,61`

#### SVC-L5. `hub_config.cpp` `read_json_file` silently swallows parse errors

**File**: `hub_config.cpp:113-126` — Returns null JSON with no log on malformed JSON.

**Fix**: Log a warning in the catch block.

#### SVC-L6. `uid_utils.hpp` `random_u32()` fallback entropy is weak

**File**: `uid_utils.hpp:80-96` — Uses `high_resolution_clock` hash. Multiple processes at the same nanosecond get the same UID.

#### SVC-L7. `FileLock` shutdown sets `g_filelock_initialized` to false without clearing `g_proc_locks`

**File**: `file_lock.cpp:733-736` — Stale entries persist if lifecycle is restarted in tests.

### SECURITY

#### SVC-S1. Vault password string not zeroed after use

**File**: `vault_crypto.cpp:76`

Password passed as `const std::string&` persists in memory after use. SSO means short passwords are stored inline on the stack.

**Fix**: Document that callers must zero password strings. Consider `std::span<const char>` or a `SecureString` wrapper.

---

#### SVC-S2. Vault salt derived from UID — may be predictable

**File**: `vault_crypto.cpp:70-73`

KDF salt is `BLAKE2b-16(uid)`. If UID is short/predictable, the only entropy comes from the password. Argon2id (64MB, ~100ms) provides moderate protection but not against dedicated GPU/ASIC attacks.

**Fix**: Document that vault security depends on password strength. Recommend minimum complexity.

---

## 4. Actor / hub_python / Scripting Subsystem

**Files reviewed (27 total)**: `src/actor/actor_api.cpp`, `actor_api.hpp`, `actor_module.cpp`, `actor_schema.cpp`, `actor_schema.hpp`, `actor_host.cpp`, `actor_host.hpp`, `actor_role_workers.cpp`, `actor_script_host.cpp`, `actor_script_host.hpp`, `actor_worker_helpers.hpp`, `actor_config.cpp`, `actor_config.hpp`, `actor_main.cpp` + `src/hub_python/pylabhub_module.hpp`, `pylabhub_module.cpp`, `admin_shell.cpp`, `admin_shell.hpp`, `python_interpreter.cpp`, `python_interpreter.hpp`, `hub_script.cpp`, `hub_script.hpp`, `hub_script_api.cpp`, `hub_script_api.hpp` + `src/scripting/python_script_host.cpp`, `python_script_host.hpp` + `src/include/utils/actor_vault.hpp`, `script_host.hpp`

### CRITICAL

#### ACT-C1. Race condition — `RoleMetrics` accessed from multiple threads without synchronization

**Files**: `actor_api.hpp:307-314`, `actor_api.cpp:163-210`
**Severity**: CRITICAL — **data race (UB)**

`RoleMetrics` fields (`script_errors`, `loop_overruns`, `last_cycle_work_us`) are plain `uint64_t` without atomic or mutex protection. They are written by the loop thread and read by Python-callable methods (`script_error_count()`, `loop_overrun_count()`, `last_cycle_work_us()`) potentially from other threads (AdminShell, stored API references). Even `++metrics_.script_errors` on a non-atomic `uint64_t` is UB if any other thread reads concurrently.

**Fix**: Make all fields `std::atomic<uint64_t>` with relaxed ordering:
```cpp
struct RoleMetrics {
    std::atomic<uint64_t> script_errors{0};
    std::atomic<uint64_t> loop_overruns{0};
    std::atomic<uint64_t> last_cycle_work_us{0};
};
```

---

#### ACT-C2. `consumer_fz_accepted_` race between `is_fz_accepted()` and `accept_flexzone_state()`

**Files**: `actor_api.hpp:317-318`, `actor_api.cpp:16-24,114-129`
**Severity**: CRITICAL — **data race on non-atomic bool**

`consumer_fz_has_accepted_` is a plain `bool` written under the GIL (by `accept_flexzone_state()`) and read without the GIL (by `is_fz_accepted()` at `actor_role_workers.cpp:1032`). Currently single-threaded (same loop thread) but the bool is not atomic — any refactoring to move the check to `zmq_thread` would create UB.

**Fix**: Make `consumer_fz_has_accepted_` an `std::atomic<bool>`.

---

#### ACT-C3. Consumer flexzone ctypes `from_buffer()` on read-only memoryview — runtime crash

**File**: `actor_role_workers.cpp:806-812`
**Severity**: CRITICAL — **runtime TypeError for all consumer ctypes flexzone schemas**

For the consumer's flexzone, a read-only memoryview is created (`readonly=true`). Then `ctypes.from_buffer(fz_mv_)` is called, but `ctypes.Structure.from_buffer()` requires a writable buffer — it raises `TypeError: underlying buffer is not writable`. The producer (line 247-253) correctly uses `readonly=false`.

**Fix**: Use `from_buffer_copy()` instead of `from_buffer()` for read-only consumer flexzone, or use a writable memoryview (flexzone is intentionally shared and writable by both parties under spinlock).

---

### HIGH

#### ACT-H1. Helper functions in anonymous namespace in a header — ODR risk and binary bloat

**File**: `actor_worker_helpers.hpp:43-473`
**Severity**: HIGH — **code duplication across TUs**

All helper functions (`is_callable`, `import_script_module`, `build_ctypes_struct`, `build_numpy_dtype`, `compute_schema_hash`, etc.) are in an anonymous namespace in a header included by both `actor_host.cpp` and `actor_role_workers.cpp`. Each TU gets its own copy of every function, including crypto calls in `compute_schema_hash`.

**Fix**: Move to a `.cpp` file or use `inline` functions in a named namespace.

---

#### ACT-H2. `g_channels_cb` static `std::function` not thread-safe

**File**: `pylabhub_module.cpp:42-48,172-178`
**Severity**: HIGH — **data race if callback re-registered at runtime**

Written by main thread during startup, read by AdminShell/hub_script threads under GIL. No mutex protection. `std::function` is not thread-safe for concurrent read/write.

**Fix**: Protect with `std::mutex`, consistent with `g_shutdown_cb` pattern in `python_interpreter.cpp:33-34`.

---

#### ACT-H3. `ActorHost::stop()` — maps accessed concurrently during clear

**File**: `actor_host.cpp:244-254`
**Severity**: HIGH — **data race on unordered_map**

In `stop()`, the GIL is released, then `producers_.clear()` / `consumers_.clear()` is called. If `is_running()` (lines 257-264) is called concurrently, it iterates the maps being cleared.

**Fix**: Guard `producers_`/`consumers_` with a mutex, or document that `is_running()` must not be called concurrently with `stop()`.

---

#### ACT-H4. `PythonInterpreter::exec()` stdout/stderr restoration not exception-safe

**File**: `python_interpreter.cpp:176-198`
**Severity**: HIGH — **stdout/stderr permanently redirected on exception**

If `io.attr("StringIO")()` or `sys.attr("stdout") = buf` throws, `old_out`/`old_err` are valid but stdout/stderr are in undefined state. Lines 196-197 may not be reached, leaving output permanently redirected.

**Fix**: Use a scope guard:
```cpp
auto restore_io = pylabhub::scope_guard([&] {
    sys.attr("stdout") = old_out;
    sys.attr("stderr") = old_err;
});
```

---

#### ACT-H5. `script_module.is_none()` checked without GIL in tick loop

**File**: `hub_script.cpp:364`
**Severity**: HIGH — **pybind11 object access without GIL**

`py::object::is_none()` accesses the internal `PyObject*` pointer. Accessing pybind11 objects without the GIL is technically undefined in pybind11's threading model.

**Fix**: Extract `bool has_script = script_loaded;` while GIL is held, use that local in the tick loop.

---

#### ACT-H6. `zmq_thread_` and `loop_thread_` use relaxed loads on `running_`

**Files**: `actor_role_workers.cpp:581,1136`
**Severity**: HIGH — **shutdown latency concern**

`running_` loaded with `relaxed` ordering in zmq_thread poll loop. The zmq_thread does not wait on `incoming_cv_` — it uses `zmq_poll`. Shutdown delay bounded by `messenger_poll_ms` (typically 5ms).

**Fix**: Use `acquire` ordering for reads, `release` for writes. Document the shutdown latency bound.

---

### MEDIUM

#### ACT-M1. `from_directory()` reads JSON file twice — TOCTOU

**File**: `actor_config.cpp:347-370`

Calls `from_json_file()` then re-opens and re-parses the same file to extract `"hub_dir"`.

**Fix**: Parse JSON once and pass the parsed object to a shared parsing function.

---

#### ACT-M2. Token comparison in AdminShell vulnerable to timing side-channel

**File**: `admin_shell.cpp:166`

Uses `std::string::operator!=` which short-circuits. Allows timing attack to determine correct token character-by-character.

**Fix**: Use `sodium_memcmp()` (libsodium already a dependency).

---

#### ACT-M3. `const_cast` to create read-only memoryview

**File**: `actor_role_workers.cpp:670-673`

`const_cast<void*>(data)` for `py::memoryview::from_memory()`. Defense-in-depth concern — Python ctypes could bypass the readonly flag.

---

#### ACT-M4. `signal_handler` accesses `g_actor_script_ptr` without synchronization

**File**: `actor_main.cpp:94-103`

Raw pointer read by signal handler. On most architectures pointer writes are atomic, but not guaranteed by the C++ standard.

**Fix**: Make `g_actor_script_ptr` a `std::atomic<ActorScriptHost*>`.

---

#### ACT-M5. `std::_Exit(1)` on double signal skips all cleanup

**File**: `actor_main.cpp:99`

Does not run destructors, flush buffers, or release SHM. Could leave spinlocks held.

---

#### ACT-M6. `HubScriptAPI` const methods return mutable back-pointers

**Files**: `hub_script_api.cpp:71,80,90,99`

`channels()` etc. are `const` but `ChannelInfo` objects have mutable back-pointer allowing `mark_for_close()`. The `const` qualifier is misleading.

---

#### ACT-M7. PyConfig cleanup may be skipped on exception

**File**: `python_script_host.cpp:58-72`

If `PyConfig_SetString` at line 71 fails, `PyConfig_Clear` is not called.

**Fix**: Check `PyStatus` return value and call `PyConfig_Clear` on failure.

---

#### ACT-M8. `ActorRoleAPI` stores raw non-owning pointers to Producer/Consumer

**File**: `actor_api.hpp:285-288`

If worker is destroyed while Python callback still references the API, pointers dangle. Current design relies on GIL synchronization during `stop()`.

---

#### ACT-M9. `ActorScriptHost` bools accessed across threads without atomics

**File**: `actor_script_host.hpp:132-138`

`script_load_ok_`, `has_active_roles_` etc. are plain bools. Currently safe due to happens-before from promise/future handoff, but fragile.

**Fix**: Add comments documenting the happens-before invariants, or make atomic.

---

### LOW

#### ACT-L1. `import_script_module()` does not guarantee module isolation

**File**: `actor_worker_helpers.hpp:64-99` — returns cached module from `sys.modules`.

#### ACT-L2. Schema field count overflow — `uint32_t` cast to `int`

**File**: `actor_worker_helpers.hpp:220-222`

#### ACT-L3. `build_messages_list_()` creates all Python objects eagerly

**File**: `actor_role_workers.cpp:106-117` — 256 messages converted at once.

#### ACT-L4. `step_write_deadline_()` does not handle clock rollback

**File**: `actor_role_workers.cpp:410-435`

#### ACT-L5. `parse_on_iteration_return()` does not increment `script_errors` on type error

**File**: `actor_worker_helpers.hpp:355-363`

#### ACT-L6. `read_password_interactive()` does not suppress echo on Windows

**File**: `actor_main.cpp:238-244`

#### ACT-L7. `reset_namespace()` ignores `PyDict_DelItem` return value

**File**: `python_interpreter.cpp:234` — pending Python exception could cause failures.

#### ACT-L8. `SchemaSpec` has `std::string` fields without ABI isolation

**File**: `actor_schema.hpp:110-122` — acceptable in current static-library build.

#### ACT-L9. `from_json_file()` uses `std::fprintf` before Logger initialized

**File**: `actor_config.cpp:301-313` — documented design choice.

#### ACT-L10. `producer_->stop()` called without GIL after thread join

**File**: `actor_role_workers.cpp:333-338` — correct (C++-only operation).

#### ACT-L11. Potential deadlock if `exec_mu` / GIL ordering is violated

**File**: `python_interpreter.cpp:170-171` — currently safe, lock order is consistent.

#### ACT-L12. `hub_script.cpp` tick loop integer division safe but unclear

**File**: `hub_script.cpp:313-316` — ternary guards against zero.

---

## 5. Public Headers / ABI / API Design

**Files reviewed (58 total)**: All headers under `src/include/utils/` (56 files), 5 umbrella headers under `src/include/` (`plh_base.hpp`, `plh_datahub.hpp`, `plh_datahub_client.hpp`, `plh_platform.hpp`, `plh_service.hpp`)

### CRITICAL

#### HDR-C1. C API header `slot_rw_coordinator.h` not compilable as C

**File**: `slot_rw_coordinator.h:26-30`
**Severity**: CRITICAL — **C consumers cannot include this header**

`namespace pylabhub::hub { ... }` block with forward declarations appears **outside** the `#ifdef __cplusplus` guard. The `namespace` keyword is invalid C syntax. All function signatures use `pylabhub::hub::SlotRWState *` — also invalid C. The `.h` extension and `extern "C"` block imply C compatibility.

**Fix**: Wrap namespace inside `#ifdef __cplusplus`, provide `typedef struct` opaque pointer typedefs for C:
```c
#ifdef __cplusplus
namespace pylabhub::hub { struct SlotRWState; struct SharedMemoryHeader; }
typedef pylabhub::hub::SlotRWState        plh_SlotRWState;
#else
typedef struct plh_SlotRWState            plh_SlotRWState;
#endif
```

---

#### HDR-C2. `recovery_api.hpp` not compilable as C despite `extern "C"` block

**File**: `recovery_api.hpp:28-31`
**Severity**: CRITICAL — **C-incompatible includes**

Includes `<cstddef>` and `<cstdint>` (C++-only headers) alongside `<stdbool.h>`. Also includes `slot_rw_coordinator.h` (HDR-C1). The `extern "C"` functions are unusable from C.

**Fix**: Use `<stddef.h>` and `<stdint.h>`. Fix HDR-C1 first.

---

#### HDR-C3. `python_loader.hpp` — `extern "C"` nested inside C++ namespace

**File**: `python_loader.hpp:86,143-146,216-220`
**Severity**: CRITICAL — **C API functions inaccessible from C**

The `extern "C"` block is nested inside `namespace pylabhub::utils`. While valid C++, a C consumer cannot reference `pylabhub::utils::PyLoader_init`. Also, `PostHistory(const std::string &s)` at line 141 lacks an export macro.

**Fix**: Move `extern "C"` block outside the namespace. Add `PYLABHUB_UTILS_EXPORT` to `PostHistory`.

---

### HIGH

#### HDR-H1. Namespace pollution via `using namespace` in header

**File**: `logger_sinks/sink.hpp:11`

`using namespace pylabhub::format_tools;` at namespace scope inside `namespace pylabhub::utils`. Every includer gets format_tools names dumped into pylabhub::utils.

**Fix**: Remove `using namespace`, qualify calls explicitly in `.cpp` files.

---

#### HDR-H2. Platform macro inconsistency: `PLATFORM_WIN64` vs `PYLABHUB_PLATFORM_WIN64`

**File**: `logger_sinks/syslog_sink.hpp:8`

Uses raw `PLATFORM_WIN64` instead of canonical `PYLABHUB_PLATFORM_WIN64`.

**Fix**: Change to `#if !defined(PYLABHUB_PLATFORM_WIN64)`.

---

#### HDR-H3. Exported structs with `std::string` members without pImpl: `KnownActor`, `ChannelPolicy`

**File**: `channel_access_policy.hpp:84-96`

`PYLABHUB_UTILS_EXPORT` structs with `std::string` data members. Violates project pImpl mandate for ABI stability.

**Fix**: Remove export macro (these are value types consumed within library) or wrap in pImpl.

---

#### HDR-H4. Exported class `ScriptHost` has STL members without pImpl

**File**: `script_host.hpp:223-232`

`PYLABHUB_UTILS_EXPORT ScriptHost` stores `std::thread`, `std::promise<void>`, `std::future<void>` directly. ABI-sensitive STL types.

**Fix**: Move into private `Impl` struct.

---

#### HDR-H5. Exported classes without pImpl: `IntegrityValidator`, `SlotDiagnostics`, `HeartbeatManager`

**Files**: `integrity_validator.hpp:43`, `slot_diagnostics.hpp:56-59`, `heartbeat_manager.hpp:58-59`

All `PYLABHUB_UTILS_EXPORT` with `std::string` or raw pointer members exposed directly.

**Fix**: Move private data into pImpl.

---

#### HDR-H6. `zmq_context.hpp` leaks ZMQ types into public API

**File**: `zmq_context.hpp:15,27`

`#include "cppzmq/zmq.hpp"` in public header. Returns `zmq::context_t &`, forcing every includer to transitively include full cppzmq.

**Fix**: Return opaque handle from public header, provide typed wrapper in internal header.

---

#### HDR-H7. `messenger.hpp` includes `nlohmann/json.hpp` in public API

**File**: `messenger.hpp:29,222`

`on_channel_error()` takes `nlohmann::json` parameter, forcing the heavy JSON header on all consumers.

**Fix**: Change parameter to `std::string` (serialized JSON) or use pImpl wrapper.

---

#### HDR-H8. `PYLABHUB_NODISCARD` macro redefined across headers

**Files**: `slot_rw_coordinator.h:18-22`, `recovery_api.hpp:33-37`

Both define `PYLABHUB_NODISCARD` unconditionally (no `#ifndef` guard). Definitions are identical but fragile.

**Fix**: Define once in `plh_platform.hpp` or `plh_macros.hpp` with `#ifndef` guard.

---

### MEDIUM

#### HDR-M1. `python_loader.hpp` defines `MAX_PATH` without prefix — clashes with Windows

**File**: `python_loader.hpp:74-79`

Defines `MAX_PATH` (260) — collides with the Windows system macro.

**Fix**: Rename to `PYLABHUB_PYLOADER_MAX_PATH`.

---

#### HDR-M2. `HOST_IMPORT` macro name too generic

**File**: `python_loader.hpp:82-84`

Likely to collide with third-party code.

**Fix**: Prefix as `PYLABHUB_HOST_IMPORT`.

---

#### HDR-M3. `DataBlockConfig` has `std::string` members, passed across DLL boundary

**File**: `data_block_config.hpp:81-150`

Five `std::string` members. Not exported itself but passed by value to exported factory functions.

---

#### HDR-M4. `ProducerInfo`/`ConsumerInfo` with `std::string` members

**File**: `messenger.hpp:47-81`

Used as parameters of exported `Messenger` methods.

---

#### HDR-M5. `ChannelSnapshotEntry`/`ChannelSnapshot` with STL containers

**File**: `broker_service.hpp:42-65`

Returned from exported `BrokerService::query_channel_snapshot()`.

---

#### HDR-M6. `BrokerService::Config` has STL containers as public members

**File**: `broker_service.hpp:70-116`

Passed to exported constructor. Consumed at construction time only (behind pImpl).

---

#### HDR-M7. `uid_utils.hpp` uses `namespace pylabhub::uid` — inconsistent with `pylabhub::utils`

**File**: `uid_utils.hpp:39`

---

#### HDR-M8. Wasted pointer storage in `SlotRef` template

**File**: `slot_ref.hpp:249-250`

Always stores both `m_write_handle` and `m_read_handle`, but only one is valid (compile-time `IsMutable`).

**Fix**: Use `std::conditional_t` to select single pointer type.

---

#### HDR-M9. Wasted pointer storage in `ZoneRef` template

**File**: `zone_ref.hpp:262-263`

Same issue as HDR-M8 — both `m_producer` and `m_consumer` stored.

---

#### HDR-M10. `SchemaValidationException` carries 64-byte payload

**File**: `schema_blds.hpp:405-416`

Two `std::array<uint8_t, 32>` public members. Atypical exception design.

---

#### HDR-M11. `timeout_constants.hpp` — potential ODR violation from per-TU macro redefinition

**File**: `timeout_constants.hpp:31-39`

`inline constexpr` vars initialized from `#ifndef`-guarded macros. If macros differ across TUs, ODR is violated.

---

#### HDR-M12. Exported thread-local variable `g_script_thread_state`

**File**: `script_host.hpp:104`

`PYLABHUB_UTILS_EXPORT extern thread_local` — portability concern for Windows/MinGW.

**Fix**: Replace with exported accessor function.

---

#### HDR-M13. `sink.hpp` `LogMessage` has `fmt::memory_buffer` — impl-specific layout in header

**File**: `logger_sinks/sink.hpp:17-24`

Currently safe (all sinks internal), but would break with external sink plugins.

---

#### HDR-M14. `LOGGER_*` macros use `__VA_OPT__` — older MSVC incompatibility

**File**: `logger.hpp:459-512`

C++20 feature. Not supported by MSVC before VS 2019 16.5+.

---

### LOW

#### HDR-L1. `plh_platform.hpp` comment says "C++17" but enforces C++20

**File**: `plh_platform.hpp:115-118`

#### HDR-L2. `data_block_fwd.hpp` includes full `data_block_config.hpp`

**File**: `data_block_fwd.hpp:14` — defeats purpose of a forward-declaration header.

#### HDR-L3. `is_initialized()` and `is_finalized()` not `const noexcept`

**File**: `lifecycle.hpp:212,221`

#### HDR-L4. Missing `[[nodiscard]]` on observer methods

**Files**: `channel_handle.hpp:114-116`, `heartbeat_manager.hpp:55`, `slot_diagnostics.hpp:39-47`

#### HDR-L5. `set_write_error_callback` takes `std::function` by value across DLL

**File**: `logger.hpp:237`

#### HDR-L6. `SchemaInfo` has `std::string` members — template-only, acceptable

**File**: `schema_blds.hpp:198-227`

#### HDR-L7. `plh_base.hpp` includes `<filesystem>` in Layer 1 umbrella

**File**: `plh_base.hpp:19` — compile-time overhead.

#### HDR-L8. `channel_pattern.hpp` and `channel_access_policy.hpp` use different namespaces

**Files**: `channel_pattern.hpp` (`pylabhub::hub`) vs `channel_access_policy.hpp` (`pylabhub::broker`)

#### HDR-L9. `recovery_api.hpp` includes `<stdbool.h>` in `.hpp` file

**File**: `recovery_api.hpp:31` — unnecessary in C++.

#### HDR-L10. `LOGGER_FMT_BUFFER_RESERVE` macro not prefixed

**File**: `logger.hpp:51-53` — should be `PYLABHUB_LOGGER_FMT_BUFFER_RESERVE`.

#### HDR-L11. `plh_datahub.hpp` comment about nlohmann/json slightly inaccurate

**File**: `plh_datahub.hpp:17-19`

---

## 6. HEP Specification vs Implementation Consistency

**Documents reviewed**: All 14 HEP documents under `docs/HEP/` cross-referenced against corresponding implementation files in `src/utils/`, `src/actor/`, `src/scripting/`, and `src/include/`

### CRITICAL

#### HEP-A1. "Lock-free queue" claim in HEP-CORE-0004 (Logger) — implementation is mutex-based

**HEP**: HEP-CORE-0004, Motivation table, Architecture Overview, Design Goals
**Implementation**: `logger.cpp:286-289,347-357`
**Severity**: CRITICAL — **spec promises safety guarantees code doesn't implement**

The HEP repeatedly describes the logger's command queue as "lock-free." The actual implementation uses `std::vector<Command> queue_` protected by `std::mutex queue_mutex_` with `std::lock_guard<std::mutex>` on every enqueue and `std::unique_lock<std::mutex>` with `condition_variable::wait()` on dequeue. Lock-free queues guarantee forward progress without blocking; this mutex-based queue does not. The HEP's "~50-200 ns enqueue latency" claim may be overstated for contended scenarios.

**Impact**: Any external analysis relying on lock-free guarantees (real-time scheduling, signal-handler usage) would be wrong. Either update the HEP or implement a proper lock-free queue.

---

### HIGH

#### HEP-D1. SlotRWState size discrepancy — 48 bytes documented, 64 bytes in memory

**HEP**: HEP-CORE-0002, Section 3.1 — "SlotRWState Array (ring_buffer_capacity x 48 bytes, cache-aligned)"
**Implementation**: `data_block.hpp:143,179-182`, `data_block.cpp:98`

`raw_size_SlotRWState == 48` (verified by `static_assert`), but `SlotRWState` is `alignas(64)`, so `sizeof(SlotRWState) == 64`. Layout code uses `sizeof(SlotRWState)` for stride calculations. Anyone computing segment sizes from the HEP spec alone underestimates the control zone by `16 * ring_buffer_capacity` bytes.

**Fix**: Update HEP to state "64 bytes per entry (48-byte payload, cache-line aligned)."

---

#### HEP-F1. FileLock typed enums replaced by boolean parameters

**HEP**: HEP-CORE-0003, Public API — `LockMode` enum (`Blocking`/`NonBlocking`), `ResourceType` enum (`File`/`Directory`)
**Implementation**: `file_lock.hpp:176-177,208-210`

HEP defines `FileLock(path, ResourceType, LockMode)`. Implementation uses `FileLock(path, bool is_directory, bool blocking)`. `FileLock(path, true, false)` is error-prone (easy to swap booleans). Code written to the HEP spec would not compile.

**Fix**: Either update HEP to match, or (preferred) introduce typed enums for type safety.

---

### MEDIUM

#### HEP-L1. ModuleDef constructor: `const char*` (HEP) vs `std::string_view` (impl)

**HEP**: HEP-CORE-0001, Public API Reference
**Implementation**: `module_def.hpp:83`

Callers using `const char*` will still compile (implicit conversion), but the spec should be updated.

---

#### HEP-L2. `set_shutdown` timeout: `unsigned int` (HEP) vs `std::chrono::milliseconds` (impl)

**HEP**: HEP-CORE-0001, Public API Reference
**Implementation**: `module_def.hpp:131`

Code written to HEP signature would fail to compile. Implementation is type-safer.

---

#### HEP-D2. `flex_zone_size` constraint contradicts actual behavior

**HEP**: HEP-CORE-0002, Section 3.1
**Implementation**: `data_block.cpp:127`, `data_block_config.hpp:122-124`

Doc says "Must be 0 or a multiple of 4096." Code silently rounds up any non-zero value and even comments "sizeof(MyType) is valid input; it will be rounded up." Self-contradictory documentation.

---

#### HEP-F2. `get_expected_lock_fullname_for` uses `bool` not `ResourceType`

**HEP**: HEP-CORE-0003
**Implementation**: `file_lock.hpp:165`

Same enum-to-bool deviation as HEP-F1.

---

#### HEP-CI1. UID namespace reference incorrect

**HEP**: HEP-CORE-0013, Section 2 — refers to `uid_utils::generate_hub_uid()`
**Implementation**: `uid_utils.hpp:39,111,125` — actual namespace is `pylabhub::uid::`

Developers searching for `uid_utils::` in code will find nothing.

---

#### HEP-CI2. "Cryptographically random" UID suffix claim — fallback is not crypto-secure

**HEP**: HEP-CORE-0013, Section 2 — "4 cryptographically random bytes"
**Implementation**: `uid_utils.hpp:80-96`

Uses `std::random_device` with fallback to `high_resolution_clock` hash when `rd.entropy() == 0`. Fallback is deterministic, not cryptographically secure.

---

#### HEP-PT1. `monotonic_time_ns` header says `high_resolution_clock`, code uses `steady_clock`

**Implementation**: `plh_platform.hpp:251`, `platform.cpp:357`

Implementation is correct (`steady_clock` is the right choice). Comment is misleading — changing code to match comment could break monotonicity.

---

### LOW

#### HEP-L3. Callback argument: `const char* data, size_t len` (HEP) vs `std::string_view` (impl)

**HEP**: HEP-CORE-0001
**Implementation**: `module_def.hpp:121,142-143`

#### HEP-L4. `DynModuleState` public enum not documented in HEP

**HEP**: HEP-CORE-0001
**Implementation**: `lifecycle_dynamic.cpp:700-706`

#### HEP-D3. Metrics section size comment says 256 bytes, actual is 280 bytes

**HEP**: HEP-CORE-0002, Section 3.1
**Implementation**: `data_block.hpp:235-276`

Total header size is enforced by `static_assert(raw_size_SharedMemoryHeader == 4096)`, so runtime behavior is correct.

#### HEP-LP2. `LoopPolicy` vs `LoopTimingPolicy` naming confusion

**HEP**: HEP-CORE-0008
**Implementation**: `data_block_policy.hpp` vs `actor_config.hpp`

Two distinct enums with similar names. Correctly documented in HEP but confusing for new developers.

---

## 7. Summary Tables

### By Subsystem

| Subsystem | CRITICAL | HIGH | MEDIUM | LOW | SECURITY | Total |
|-----------|----------|------|--------|-----|----------|-------|
| DataBlock/SHM | 2 | 6 | 8 | 5 | 2 | 23 |
| IPC/Messenger/Broker/Hub | 3 | 7 | 8 | 7 | 0 | 25 |
| Service/Logging/Config/Crypto | 3 | 5 | 8 | 7 | 2 | 25 |
| Actor/hub_python/Scripting | 3 | 6 | 9 | 12 | 0 | 30 |
| Public Headers/ABI | 3 | 8 | 14 | 11 | 0 | 36 |
| HEP Consistency | 1 | 2 | 7 | 4 | 0 | 14 |
| **Total** | **15** | **28** | **47** | **35** | **4** | **153** |

### Top Priority Fixes

1. **Secret key zeroing** (IPC-C1, SVC-C1, SVC-C2, SVC-C3, IPC-H2, SVC-S1) — Straightforward to fix with `sodium_memzero` calls and scope guards. Affects all CurveZMQ keys, vault encryption keys, and admin tokens.

2. **Thread capture of `this`** (IPC-C3) — Change thread lambdas to capture `pImpl.get()` instead of `this`.

3. **ZMQ context startup race** (IPC-C2) — Use `std::call_once`.

4. **Heartbeat registration data corruption** (SHM-C1) — Move `memcpy` after CAS success.

5. **Write index gap on timeout** (SHM-C2) — Mark timed-out slots as skipped, or defer index increment.

6. **Consumer flexzone ctypes crash** (ACT-C3) — Use `from_buffer_copy()` for read-only consumer flexzone.

7. **C API headers not C-compatible** (HDR-C1, HDR-C2, HDR-C3) — Fix namespace guards, use C-compatible includes.

8. **RoleMetrics data race** (ACT-C1) — Make metrics fields `std::atomic<uint64_t>`.

9. **assert→throw migration** (IPC-H1) — Systematic replacement for lifecycle-dependent checks.

10. **pImpl violations** (HDR-H3, HDR-H4, HDR-H5, SHM-H6) — Audit exported classes for ABI compliance.

11. **Callback data races** (IPC-H3, ACT-H2) — Enforce "set before start()" with runtime checks or add mutexes.

12. **HEP accuracy** (HEP-A1, HEP-D1, HEP-F1) — Update specs to match implementation (lock-free claim, SlotRWState size, FileLock API).
