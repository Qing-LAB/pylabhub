# REVIEW-C (#276) — HEP-CORE-0041 SHM Channel Auth

**Milestone:** REVIEW-C — the post-#275-deletion systematic review that gates REVIEW-D.
**Date:** 2026-07-16 · **Status:** ✅ COMPLETE — all items resolved.

## Scope

REVIEW-C had two charges:
1. Confirm #275 legacy-secret / ShmQueue deletion is complete (nothing reachable from production).
2. Re-examine the 5 medium-tier items carried from REVIEW-B. Their original agent finding-text was lost in a context compaction (see `docs/archive/transient-2026-06-27/todo-completions/AUTH_TODO_completions.md:335`), so each was re-derived from current code and ruled REAL / already-fixed / non-issue.

## Findings

| # | Item | Verdict | Resolution |
|---|------|---------|------------|
| 1 | SCM_RIGHTS multi-fd / `MSG_CTRUNC` defense (`shm_capability_channel.cpp`) | ⚠ LOW — defense-in-depth, **not** a live fd-leak | **FIXED** — added a fail-closed `MSG_CTRUNC` check after the recv loop |
| 2 | `MemfdConsumer` ctor RAII (asymmetric cleanup on ctor failure) | ✅ NON-ISSUE | Cleanup is symmetric on every throw path (`SockCloser` RAII first; explicit `close` before each throw; members assigned last). The prior fd-leak fix was in the DataBlock **producer** ctor, a different class. |
| 3 | HEP §6.4 "missing" factory `create_reader_standby` | ✅ NON-ISSUE | Factory exists + wired (`hub_shm_queue.cpp:190`, dispatched `:265`). §6.4 never claimed it. Item premise was wrong. |
| 4 | HEP §11 "Next steps" + §10.1 sub-status stale | ⚠ doc-hygiene | **FIXED** — flipped 1i-mig-4 (#272), 1i-mig-5, 1i-cleanup (#275), REVIEW-B, 1j (#257), 1k (#258) to ✅; §11 narrowed to REVIEW-C/D/E gates |
| 5 | D3 consumer-dial retry budget (`role_api_base.cpp:1604-1653`) | ⚠ partial — real test gap | **FIXED** — the *duration* budget (10×100ms < 2000ms ceiling) is correctly a static timer-chain (no runtime test), but the *control flow* (nullopt-on-connect-failure → retry vs throw-on-protocol-error → bail) had no L2 test. Added `AttachProtocolTest.ConnectToUnboundEndpoint_ReturnsNulloptNotThrow` + `EmptyEndpoint_ThrowsNotNullopt`. |
| 6 | #275 deletion completeness | ✅ CLEAN | Zero live residue in `src/`. Only deletion-doc comments, the intentional renamed `reserved_capability_token` byte-slot, and the `kRetiredKeys[]` rejection guard. |

## Details on the two code fixes

### Item 1 — `MSG_CTRUNC` hardening
`recvmsg` at `shm_capability_channel.cpp:617` parses a single SCM_RIGHTS fd from a control buffer sized `CMSG_SPACE(sizeof(int))` with a strict `cmsg_len == CMSG_LEN(sizeof(int))` check. This is *not* the hypothesised leak: on Linux `scm_detach_fds` installs only the first fd, discards the rest, and sets `MSG_CTRUNC` — so extra fds never enter the process and the extracted fd is always valid. But a truncated multi-fd message was *silently accepted* (first fd taken). Given the project's fail-closed frame, a truncated ancillary message is now rejected:
```cpp
if (msg.msg_flags & MSG_CTRUNC)
    throw std::runtime_error("... truncated ancillary data (MSG_CTRUNC) ...");
```
Cannot affect the legitimate one-fd path (which never sets `MSG_CTRUNC`).

### Item 5 — D3 retry control-flow coverage
The dial loop depends on `initiate_consumer_handshake` returning `std::nullopt` on `ENOENT`/`ECONNREFUSED` (the H3a race — REG_ACK beats the producer's bind → retry) and throwing on a boundary/protocol error (→ bail). Contract site: `attach_protocol.cpp:320-331` (connect) + `:280-308` (boundary). Two L2 tests now pin both sides.

## Outcome

All six items resolved. #275 deletion confirmed complete. Full ctest 2553/2553.
**REVIEW-C ✅ → REVIEW-D (#277) unblocked.**
