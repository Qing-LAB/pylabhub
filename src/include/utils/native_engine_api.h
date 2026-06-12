/**
 * @file native_engine_api.h
 * @brief pylabHub native engine API — C-linkage interface for native script engines.
 *
 * Native engine authors include this header and implement the required symbols.
 * All exported symbols use C linkage for ABI stability across compilers.
 *
 * The native engine receives a PlhNativeContext from the framework at init time.
 * All framework services (logging, metrics, shutdown, hub control) are accessed
 * through function pointers on the context — no host symbol resolution needed.
 * The same plugin can run on EITHER side of the wire: role-side (default,
 * receives a `RoleAPIBase`-backed ctx) or hub-side (receives a `HubAPI`-backed
 * ctx with the 16 `hub_*` fn ptrs wired through).  See `is_hub()` predicate
 * in the C++ wrapper and §4.9 in HEP-CORE-0028.
 *
 * Current ABI version: see `PLH_NATIVE_API_VERSION` below.  v7 (2026-06-11)
 * is the latest; v3..v6 history is in the version log.
 *
 * ## Minimal Producer Native engine (C)
 *
 * ```c
 * #include <pylabhub/native_engine_api.h>
 *
 * static const PlhNativeContext *g_ctx;
 *
 * PLH_EXPORT bool native_init(const PlhNativeContext *ctx) { g_ctx = ctx; return true; }
 * PLH_EXPORT void native_finalize(void) { g_ctx = NULL; }
 *
 * PLH_EXPORT bool on_produce(const plh_tx_t *tx) {
 *     float *slot = (float *)tx->slot;
 *     slot[0] = 42.0f;
 *     g_ctx->log(g_ctx, PLH_LOG_INFO, "produced a value");
 *     return true;
 * }
 * ```
 *
 * See HEP-CORE-0028 for the full specification.
 * See docs/README/README_NativePlugin.md for a developer guide.
 */

#ifndef PYLABHUB_NATIVE_ENGINE_API_H
#define PYLABHUB_NATIVE_ENGINE_API_H

#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Platform export macro
 * ========================================================================= */

#if defined(_WIN32) || defined(_WIN64)
#   define PLH_EXPORT __declspec(dllexport)
#else
#   define PLH_EXPORT __attribute__((visibility("default")))
#endif

/* =========================================================================
 * Log level constants
 * ========================================================================= */

#define PLH_LOG_DEBUG  0
#define PLH_LOG_INFO   1
#define PLH_LOG_WARN   2
#define PLH_LOG_ERROR  3

/* =========================================================================
 * Enumerated-return macros (API v6 #194 Phase C, 2026-06-11)
 *
 * Every "one-of-N state" return value on PlhNativeContext is surfaced as
 * an int + preprocessor macro pair.  Pure C does not have type-safe
 * enums (compilers may widen `enum` to different `int` widths), so the
 * stable wire form is `int` + macro.  The C++ wrapper layer derives
 * `enum class : int` types from these macros for type-safe C++ use
 * (see the C++ convenience layer at the bottom of this file).
 *
 * See HEP-CORE-0028 §2.4 (Two-Layer Plugin Authoring Model) for the
 * design rationale + HEP-CORE-0028 §4.1 for the per-fn-ptr mapping.
 * ========================================================================= */

/** Negotiated CURVE mechanism for a queue side (HEP-CORE-0035 §2). */
#define PLH_MECHANISM_UNINITIALIZED   0  /**< queue not started */
#define PLH_MECHANISM_CURVE           1  /**< CURVE engaged */

/** Queue overflow / mode policy (HEP-CORE-0007).  Maps to the strings
 *  reported by Python `api.out_policy()` / Lua `api.out_policy()`. */
#define PLH_QUEUE_POLICY_UNKNOWN     0   /**< unconnected / unrecognized */
#define PLH_QUEUE_POLICY_SHM         1   /**< shm_read or shm_write */
#define PLH_QUEUE_POLICY_ZMQ_DROP    2   /**< zmq_push_drop */
#define PLH_QUEUE_POLICY_ZMQ_BLOCK   3   /**< zmq_push_block */
#define PLH_QUEUE_POLICY_ZMQ_RING    4   /**< zmq_pull_ring_N */

/** Reason the role host was asked to stop (HEP-CORE-0001).  Values mirror
 *  `RoleHostCore::StopReason`; widths match the existing `stop_reason_string()`
 *  mapping so Python/Lua + Native plugins observe the same codomain. */
#define PLH_STOP_REASON_NORMAL          0
#define PLH_STOP_REASON_PEER_DEAD       1
#define PLH_STOP_REASON_HUB_DEAD        2
#define PLH_STOP_REASON_CRITICAL_ERROR  3
#define PLH_STOP_REASON_CHANNEL_CLOSED  4
#define PLH_STOP_REASON_SCRIPT_ERROR    5

/* hub_post_event return values (API v7 #84): preserve the tristate the
 * C ABI emits so plugin authors can distinguish "the event name is
 * malformed" from "the broker / control loop is unhealthy".  The C++
 * wrapper surfaces these as the `PostEventResult` enum class. */
#define PLH_POST_EVENT_ACCEPTED        1
#define PLH_POST_EVENT_INVALID_NAME    0
#define PLH_POST_EVENT_TRANSPORT_ERROR (-1)

/* Invoke direction structs (plh_rx_t, plh_tx_t, plh_inbox_msg_t) +
 * visitor + arg-struct typedefs (plh_allowed_peer_t,
 * plh_allowed_peer_visitor, plh_band_member_t, plh_band_member_visitor,
 * plh_allowlist_changed_args_t, ...). */
#include "native_invoke_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Forward declaration for self-referencing function pointers
 * ========================================================================= */
struct PlhNativeContext;

/* =========================================================================
 * Native engine context — passed to native_init()
 *
 * Contains identity strings (read-only) AND framework API function pointers.
 * The native engine calls framework services through these pointers — no host
 * symbol resolution (dlsym) needed, no -rdynamic required.
 *
 * All function pointers take `const struct PlhNativeContext *ctx` as first
 * argument for context routing. The caller just passes `ctx` through.
 * ========================================================================= */

/** Magic number for PlhNativeContext validation: 'P','L','H','C' = 0x504C4843 */
#define PLH_CONTEXT_MAGIC 0x504C4843u

/** Explicit 8-byte alignment for stable struct layout across compilers. */
typedef struct
#ifdef __cplusplus
    alignas(8)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Alignas(8)
#endif
    PlhNativeContext
{
    uint32_t _magic;           /**< Must be PLH_CONTEXT_MAGIC. Validates pointer. */

    /* ── Identity (read-only, valid until native_finalize) ──────────── */
    const char *role_tag;      /**< "prod", "cons", or "proc" */
    const char *uid;           /**< Role UID */
    const char *name;          /**< Role name */
    const char *channel;       /**< Primary channel (in_channel for processor) */
    const char *out_channel;   /**< Output channel (processor only; NULL otherwise) */
    const char *log_level;     /**< Configured log level string */
    const char *role_dir;      /**< Role directory path */

    /* ── Framework API (function pointers filled by host) ──────────── */

    /** Log a message. @param level PLH_LOG_DEBUG/INFO/WARN/ERROR */
    void (*log)(const struct PlhNativeContext *ctx, int level, const char *msg);

    /** Report a custom metric (key-value pair). */
    void (*report_metric)(const struct PlhNativeContext *ctx,
                          const char *key, double value);

    /** Clear all custom metrics. */
    void (*clear_custom_metrics)(const struct PlhNativeContext *ctx);

    /** Request graceful stop (signals main loop to exit after current iteration). */
    void (*request_stop)(const struct PlhNativeContext *ctx);

    /** Signal a critical error (sets critical flag + requests stop).
     *  @param msg  REQUIRED — null-terminated string describing the
     *              unrecoverable condition.  Logged at ERROR level by
     *              the host as `[role_tag/uid] CRITICAL: <msg>` BEFORE
     *              flipping state so log scrapers see the message
     *              adjacent to the stop event.  Passing NULL is a
     *              plugin bug; the host tolerates it (logs
     *              "(no message)" placeholder) but plugin authors
     *              should always pass a real string.  Uniform with
     *              the Python `api.set_critical_error(msg)` and Lua
     *              `api.set_critical_error(msg)` surfaces (audit S2,
     *              2026-05-18).  API v3 — incompatible with v2
     *              plugins (which called this with one arg). */
    void (*set_critical_error)(const struct PlhNativeContext *ctx,
                               const char *msg);

    /** Check if critical error has been flagged. Returns 1 or 0. */
    int (*is_critical_error)(const struct PlhNativeContext *ctx);

    /** Get stop reason as a PLH_STOP_REASON_* macro value
     *  (NORMAL=0, PEER_DEAD=1, HUB_DEAD=2, CRITICAL_ERROR=3,
     *  CHANNEL_CLOSED=4, SCRIPT_ERROR=5).  API v6 (#194 Phase C) —
     *  replaces v5 const char * return.  See PLH_STOP_REASON_* macros
     *  at the top of this header. */
    int (*stop_reason)(const struct PlhNativeContext *ctx);

    /** Query counters (consistent names across all engines). */
    uint64_t (*out_slots_written)(const struct PlhNativeContext *ctx);
    uint64_t (*in_slots_received)(const struct PlhNativeContext *ctx);
    uint64_t (*out_drop_count)(const struct PlhNativeContext *ctx);
    uint64_t (*script_error_count)(const struct PlhNativeContext *ctx);
    uint64_t (*loop_overrun_count)(const struct PlhNativeContext *ctx);
    uint64_t (*last_cycle_work_us)(const struct PlhNativeContext *ctx);

    /** Spinlock — side: PLH_SIDE_TX (0), PLH_SIDE_RX (1), or -1 for auto.
     *  lock returns 1 on success, 0 on timeout. timeout_ms: -1=infinite, 0=try, >0=wait. */
    int      (*spinlock_lock)(const struct PlhNativeContext *ctx, int index, int side, int timeout_ms);
    void     (*spinlock_unlock)(const struct PlhNativeContext *ctx, int index, int side);
    uint32_t (*spinlock_count)(const struct PlhNativeContext *ctx, int side);
    /** Check if spinlock is held by this process. Returns 1 or 0. */
    int      (*spinlock_is_locked)(const struct PlhNativeContext *ctx, int index, int side);

    /** Schema sizes (logical = C struct size). side: PLH_SIDE_TX/RX or -1 for auto. */
    size_t   (*slot_logical_size)(const struct PlhNativeContext *ctx, int side);
    size_t   (*flexzone_logical_size)(const struct PlhNativeContext *ctx, int side);

    /** Role discovery. Returns 1 if found, 0 on timeout. */
    int      (*wait_for_role)(const struct PlhNativeContext *ctx, const char *uid, int timeout_ms);

    /* ── Band pub/sub (HEP-CORE-0030) — API v6 visitor + inquiry ────
     * Surface uniformity with the allowed-peers cluster below: same
     * visitor + contains + count triplet on `band_members`, same
     * lifetime contract on the visitor argument struct.  See
     * HEP-CORE-0028 §4.1 + §4.8. */

    /** band_join: returns 1 on success, 0 on rejection (broker
     *  returned `{status:error}`), -1 on transport / argument error.
     *  API v6 drops the list-from-join behaviour — plugin calls
     *  `band_members()` separately to enumerate after a successful
     *  join. */
    int  (*band_join)(const struct PlhNativeContext *ctx, const char *channel);

    /** band_leave: returns 1 on success, 0 on rejection (NOT_A_MEMBER
     *  or other broker error_code), -1 on transport / argument
     *  error. */
    int  (*band_leave)(const struct PlhNativeContext *ctx, const char *channel);

    /** band_broadcast: body_json is a JSON string. Fire-and-forget;
     *  no status return.  Status-return improvement is a cross-engine
     *  concern tracked under #192. */
    void (*band_broadcast)(const struct PlhNativeContext *ctx,
                           const char *channel, const char *body_json);

    /** band_members: visit every current member of the band.  Returns
     *  the count visited (>=0), or -1 on error (null arg / unknown
     *  channel / visitor threw — host catches exceptions and aborts
     *  iteration; see HEP-CORE-0028 §4.8 noexcept rule).  Visitor
     *  arguments follow the lifetime contract on plh_band_member_t. */
    int  (*band_members)(const struct PlhNativeContext *ctx,
                         const char *channel,
                         plh_band_member_visitor visitor,
                         void *userdata);

    /** band_member_contains: 1 if `role_uid` is a current band member,
     *  0 if not, -1 on error (null arg / unknown channel).
     *
     *  ⚠ COST: each call issues a fresh broker REQ round-trip via the
     *  underlying `band_members` RPC (unlike the allowed_peers cluster
     *  which serves from a local push-cache).  For repeated membership
     *  queries in a hot path, use `band_members(...)` once with a
     *  visitor that builds a local std::unordered_set (or the C++
     *  `plh::BandHandle::to_uid_set()` helper) and query that. */
    int  (*band_member_contains)(const struct PlhNativeContext *ctx,
                                 const char *channel,
                                 const char *role_uid);

    /** band_member_count: current band member count (>=0) or -1 on
     *  error.  Same RPC cost as band_member_contains above. */
    int  (*band_member_count)(const struct PlhNativeContext *ctx,
                              const char *channel);

    /* ── Channel-auth observability (HEP-CORE-0036 §I11 + §6.7) ─────
     * Same visitor + inquiry triplet shape as band membership above
     * — learn one pattern, know both. */

    /** allowed_peers: visit every currently authorized peer for
     *  `channel`.  Returns count (>=0) or -1 on error.  Visitor
     *  argument follows the lifetime contract on plh_allowed_peer_t.
     *  Visitor MUST be noexcept (HEP-CORE-0028 §4.8). */
    int  (*allowed_peers)(const struct PlhNativeContext *ctx,
                          const char *channel,
                          plh_allowed_peer_visitor visitor,
                          void *userdata);

    /** allowed_peer_contains: 1 if `role_uid` is in the channel's
     *  authorized-peer set, 0 if not, -1 on error. */
    int  (*allowed_peer_contains)(const struct PlhNativeContext *ctx,
                                  const char *channel,
                                  const char *role_uid);

    /** allowed_peer_count: count of authorized peers (>=0) or -1 on
     *  error. */
    int  (*allowed_peer_count)(const struct PlhNativeContext *ctx,
                               const char *channel);

    /** is_channel_ready: returns 1 iff the queue serving `channel` is
     *  in the HEP-0036 §6.7 Active state (start() succeeded
     *  post-Configured gate), 0 otherwise, -1 on error (null arg). */
    int  (*is_channel_ready)(const struct PlhNativeContext *ctx,
                             const char *channel);

    /** queue_mechanism: returns a PLH_MECHANISM_* macro value
     *  (UNINITIALIZED=0, CURVE=1) for the named side.  API v6 #194
     *  Phase C — replaces v5 const char * return; see PLH_MECHANISM_*
     *  macros at top of header.  HEP-CORE-0035 §2 CURVE-unconditional
     *  invariant constrains the codomain. */
    int  (*queue_mechanism)(const struct PlhNativeContext *ctx, int side);

    /* ── Queue diagnostics: depth + policy + receive seq ──────────── */

    /** Output (PUSH) ring buffer slot count.  0 when no tx side. */
    uint64_t (*out_capacity)(const struct PlhNativeContext *ctx);
    /** Input (PULL) ring buffer slot count.  0 when no rx side. */
    uint64_t (*in_capacity)(const struct PlhNativeContext *ctx);

    /** Output overflow policy as PLH_QUEUE_POLICY_* macro value.  API
     *  v6 #194 Phase C — replaces v5 char* return.  Returns
     *  PLH_QUEUE_POLICY_UNKNOWN when no tx side or unrecognized. */
    int (*out_policy)(const struct PlhNativeContext *ctx);
    /** Input overflow policy as PLH_QUEUE_POLICY_* macro value. */
    int (*in_policy)(const struct PlhNativeContext *ctx);

    /** Wire frame sequence number of the last decoded slot (rx side).
     *  0 until first successful read.  ZmqQueue: most recently decoded
     *  frame.  ShmQueue: last slot consumed via read_acquire. */
    uint64_t (*last_seq)(const struct PlhNativeContext *ctx);

    /* ── Metrics snapshot (API v6 #194 Phase C — opaque + key lookup) ─
     * Replaces v5 metrics_json (which returned a malloc'd JSON string).
     * Two-call pattern: snapshot once (builds a host-owned thread-local
     * cache), then look up keys against the opaque pointer. */

    /** metrics_snapshot: build a thread-local snapshot of the current
     *  role-metrics tree (HEP-CORE-0019) and return an opaque view
     *  pointer.
     *
     *  Lifetime contract (HEP-CORE-0028 §4.8):
     *    - Valid until the next metrics_snapshot() call on the SAME
     *      thread.
     *    - Plugin MUST NOT share across threads.
     *    - Plugin MUST NOT retain the pointer across the engine's
     *      `native_finalize()` callback — on the same thread, a
     *      subsequently-loaded plugin would alias the cache and any
     *      retained pointer would silently read the new plugin's
     *      metrics.  Drop the pointer in on_stop().
     *
     *  Returns NULL on internal error. */
    const void *(*metrics_snapshot)(const struct PlhNativeContext *ctx);

    /** metrics_get: dotted-path metric lookup against a snapshot
     *  pointer (key syntax: "queue.tx.drop_count" etc., matching the
     *  JSON tree returned by Python `api.metrics()`).  Returns 1 if
     *  found (writes value into *out), 0 if missing, -1 on error
     *  (null snap / null out / null key). */
    int (*metrics_get)(const void *snapshot,
                       const char *key,
                       double *out);

    /* ── Flexzone control + band-membership query ──────────────────── */

    /** is_in_band: 1 iff this role is a current member of `channel`'s
     *  band routing table (HEP-CORE-0030 §5.3 S4).  Local-cache
     *  query (no broker round trip). */
    int (*is_in_band)(const struct PlhNativeContext *ctx, const char *channel);

    /** update_flexzone_checksum: SHM-only.  Recomputes + stores the
     *  flexzone checksum after the plugin mutated fz contents.  Returns
     *  1 on success, 0 on no-op (e.g. ZMQ side or no fz wired). */
    int (*update_flexzone_checksum)(const struct PlhNativeContext *ctx);

    /** set_verify_checksum: SHM-only.  Enables/disables per-read slot +
     *  flexzone checksum verification.  ZMQ side: silent no-op. */
    void (*set_verify_checksum)(const struct PlhNativeContext *ctx, int enable);

    /* ── Hub-side API surface (API v7 #84, 2026-06-11) ─────────────────
     * These fn ptrs are wired only by NativeEngine::build_api_(HubAPI&)
     * (i.e. when `script.type = "native"` in hub.json).  On role-side
     * contexts they are noop-stubs returning the documented sentinel.
     * On hub-side contexts the role-side surfaces (band_*, allowed_*,
     * spinlock_*, queue_*, slot_*, etc.) are also stubbed for symmetry
     * — see HEP-CORE-0028 §4.9 "Hub-side API surface" for the full
     * matrix.
     *
     * Each JSON-returning fn writes into a thread-local `std::string`
     * buffer and returns its `c_str()`.  Lifetime contract: pointer
     * valid until the NEXT hub_* JSON call on the SAME thread (any
     * subsequent hub_metrics_json / hub_config_json / hub_get_*_json /
     * hub_query_metrics_json / hub_list_*_json overwrites the same
     * buffer).  Plugin MUST parse or strdup before the next call.
     *
     * Return contract: ALWAYS a valid C string — never NULL.
     * On error (null ctx, null _api, HubAPI threw) the scratch is
     * cleared and an empty `""` is returned.  Plugin authors can omit
     * null-checks on the return; distinguish "no data" from "error"
     * via string emptiness + log signals. */

    /** hub_metrics_json: returns the hub metrics tree as a JSON string.
     *  Never NULL — empty `""` on error or wrong-side stub.  See the
     *  Return contract block above for shared lifetime semantics. */
    const char *(*hub_metrics_json)(const struct PlhNativeContext *ctx);

    /** hub_config_json: returns the hub config as a JSON string. */
    const char *(*hub_config_json)(const struct PlhNativeContext *ctx);

    /** hub_query_metrics_json: filtered metrics by category list (JSON
     *  array of strings as input; empty = all). */
    const char *(*hub_query_metrics_json)(const struct PlhNativeContext *ctx,
                                          const char *categories_json);

    /** hub_list_channels_json: JSON array of channel descriptors. */
    const char *(*hub_list_channels_json)(const struct PlhNativeContext *ctx);

    /** hub_get_channel_json: channel info by name; "{}" if absent. */
    const char *(*hub_get_channel_json)(const struct PlhNativeContext *ctx,
                                        const char *name);

    /** hub_list_roles_json: JSON array of role descriptors. */
    const char *(*hub_list_roles_json)(const struct PlhNativeContext *ctx);

    /** hub_get_role_json: role info by uid; "{}" if absent. */
    const char *(*hub_get_role_json)(const struct PlhNativeContext *ctx,
                                     const char *role_uid);

    /** hub_list_bands_json: JSON array of band descriptors. */
    const char *(*hub_list_bands_json)(const struct PlhNativeContext *ctx);

    /** hub_get_band_json: band info by name; "{}" if absent. */
    const char *(*hub_get_band_json)(const struct PlhNativeContext *ctx,
                                     const char *name);

    /** hub_list_peers_json: JSON array of federation peer descriptors. */
    const char *(*hub_list_peers_json)(const struct PlhNativeContext *ctx);

    /** hub_get_peer_json: peer info by hub_uid; "{}" if absent. */
    const char *(*hub_get_peer_json)(const struct PlhNativeContext *ctx,
                                     const char *hub_uid);

    /** hub_close_channel: control delegate.  1 on accept, -1 on error.
     *  Idempotent for unknown names (broker tolerates). */
    int (*hub_close_channel)(const struct PlhNativeContext *ctx,
                             const char *name);

    /** hub_broadcast_channel: send a control-plane broadcast to all
     *  consumers of a channel.  1 on accept, -1 on error. */
    int (*hub_broadcast_channel)(const struct PlhNativeContext *ctx,
                                 const char *channel,
                                 const char *message,
                                 const char *data_json);

    /** hub_post_event: post a user-defined event onto the worker's
     *  main-loop queue (fires on_app_<name>(api, data) on the worker
     *  thread).  Name MUST be a valid identifier per HEP-CORE-0033
     *  G2.2.0b.  1 on accept, 0 on invalid name, -1 on error. */
    int (*hub_post_event)(const struct PlhNativeContext *ctx,
                          const char *name,
                          const char *data_json);

    /** hub_augment_timeout_ms: current augment timeout knob.
     *  Convention: -1 = infinite, 0 = non-blocking, >0 = N ms. */
    int64_t (*hub_augment_timeout_ms)(const struct PlhNativeContext *ctx);

    /** hub_set_augment_timeout: setter for augment_timeout_ms. */
    void (*hub_set_augment_timeout)(const struct PlhNativeContext *ctx,
                                    int64_t ms);

    /* ── Opaque host data (do not dereference) ────────────────────── */
    void *_core;               /**< Internal — RoleHostCore pointer for API implementations. */
    void *_api;                /**< Internal — RoleAPIBase pointer for spinlock/messaging. */
    const char *_log_label;    /**< Internal — log prefix e.g. "[native libfoo.so]" */

    uint32_t _magic_end;       /**< Must be PLH_CONTEXT_MAGIC. Trailing sentinel. */

} PlhNativeContext;

/* =========================================================================
 * ABI compatibility info — exported by native engine
 * ========================================================================= */

typedef struct PlhAbiInfo
{
    uint32_t struct_size;       /**< sizeof(PlhAbiInfo) — versioning guard */
    uint32_t sizeof_void_ptr;   /**< sizeof(void*) — 4 or 8 */
    uint32_t sizeof_size_t;     /**< sizeof(size_t) */
    uint32_t byte_order;        /**< 1 = little-endian, 2 = big-endian */
    uint32_t api_version;       /**< PLH_NATIVE_API_VERSION */

    /* ─────────────────────────────────────────────────────────────────
     * Fields below added in struct_size >= this struct's size
     * (HEP-CORE-0032, 2026-04-22).  Host uses struct_size to decide
     * whether these are present; plugins compiled against older
     * headers still produce a smaller struct_size and the host skips
     * the extra checks.
     *
     * ComponentVersions: fingerprint of pylabhub-utils headers the
     * plugin was compiled against.  Mostly diagnostic — plugins use
     * PlhNativeContext callbacks, not the C++ surfaces these axes
     * cover, so a mismatch here is informational rather than fatal.
     * Exception: build_id drift in strict mode is fatal, same as for
     * the main binary.
     * ───────────────────────────────────────────────────────────────── */

    uint16_t library_major;
    uint16_t library_minor;
    uint16_t library_rolling;

    uint8_t  shm_major,           shm_minor;
    uint8_t  broker_proto_major,  broker_proto_minor;
    uint8_t  zmq_frame_major,     zmq_frame_minor;
    uint8_t  script_api_major,    script_api_minor;
    uint8_t  script_engine_major, script_engine_minor;
    uint8_t  config_major,        config_minor;

    /** Null-terminated pylabhub build_id the plugin was compiled against,
     *  or empty string if plugin was built without build-id support. */
    char     build_id[48];
} PlhAbiInfo;

/** Current native engine API version. Increment on breaking changes
 *  to the call surface (new/removed required symbols, changed function
 *  signatures).
 *
 *  Version log:
 *    v2 → v3 (audit S2, 2026-05-18): `set_critical_error` function
 *           pointer gained a `const char *msg` parameter (uniform with
 *           the Python `api.set_critical_error([msg])` and Lua
 *           `api.set_critical_error(msg)` surfaces).  Plugins built
 *           against v2 will be rejected by the host's `verify_abi_()`
 *           with a clear ABI-mismatch error — rebuild against this
 *           header.
 *    v3 → v4 (#194 Phase A, 2026-06-10): PlhNativeContext gains
 *           `allowed_peers` / `is_channel_ready` / `queue_mechanism`
 *           function pointers (HEP-CORE-0036 §I11 + §6.7 parity with
 *           Lua/Python).  New `plh_allowlist_changed_args_t` +
 *           `plh_allowed_peer_t` for the `on_allowlist_changed`
 *           plugin-side callback.  Plugins built against v3 will be
 *           rejected — rebuild against this header.  v3 plugins
 *           silently lacked these capabilities (no compile error;
 *           runtime behaviour was as if the host didn't expose the
 *           §I11 surface at all).
 *    v4 → v5 (#194 Phase B2, 2026-06-10): PlhNativeContext gains
 *           `out_capacity` / `out_policy` / `in_capacity` /
 *           `in_policy` / `last_seq` function pointers — diagnostic
 *           parity with Lua + Python (which already exposed these
 *           via per-side closures / `.def`s).  Plugins built against
 *           v4 will be rejected — rebuild against this header.
 *    v5 → v6 (#194 Phase C, 2026-06-11): Two-Layer Plugin Authoring
 *           Model cleanup — replaces every malloc-returning fn ptr
 *           and every `const char *` enumerated return with int +
 *           PLH_*_* macro / visitor / opaque-snapshot pattern.  No
 *           allocations cross the C ABI boundary; no exceptions
 *           cross the boundary either.  Signatures changed for:
 *           `stop_reason` (`const char *` → `int`); `queue_mechanism`
 *           (`const char *` → `int`); `out_policy` / `in_policy`
 *           (`char *` malloc → `int`); `band_join` (`char *` malloc
 *           → `int`); `band_members`, `allowed_peers` (`char *`
 *           malloc → visitor pattern returning count).  Added:
 *           `band_member_contains` / `band_member_count` /
 *           `allowed_peer_contains` / `allowed_peer_count` (inquiry
 *           helpers); `metrics_snapshot` + `metrics_get` (replacing
 *           `metrics_json`).  Plugins built against v5 will be
 *           rejected with a clear ABI-mismatch error — rebuild
 *           against this header.  See HEP-CORE-0028 §2.4 (Two-Layer
 *           Plugin Authoring Model) + §4.1 (PlhNativeContext) +
 *           §4.8 (Lifetime + Security Contract).
 *
 *    v6 → v7 (#84 task: NativeEngine::build_api_(HubAPI&) — extend
 *           beyond MVP; 2026-06-11): hub-side surface parity with
 *           Python/Lua.  PlhNativeContext gains 16 new fn ptrs at the
 *           end of the struct (additive) wiring through to HubAPI:
 *           hub_metrics_json / hub_config_json / hub_query_metrics_json,
 *           hub_list_channels_json / hub_get_channel_json (and the
 *           4 list/get pairs for roles/bands/peers),
 *           hub_close_channel / hub_broadcast_channel / hub_post_event,
 *           hub_augment_timeout_ms / hub_set_augment_timeout.
 *           wire_hub() now assigns EVERY ctx fn ptr — role-only
 *           methods get noop-stubs returning documented sentinels so
 *           pure-C hub plugins cannot segfault from missing null
 *           checks.  See HEP-CORE-0028 §4.9 "Hub-side API surface"
 *           for the full matrix.
 *
 *  Additive PlhAbiInfo fields are NOT breaking — they're
 *  guarded by struct_size. */
#define PLH_NATIVE_API_VERSION 7

/* =========================================================================
 * C-visible pylabhub ComponentVersions constants
 *
 * Mirror the C++ `inline constexpr` values in `plh_version_registry.hpp`.
 * A static_assert in version_registry.cpp pins the two locations equal.
 * C plugins use these to populate their PlhAbiInfo.
 * ========================================================================= */

#define PLH_COMPONENT_SHM_MAJOR            1
#define PLH_COMPONENT_SHM_MINOR            0
/* broker_proto 1 → 2 (Wave M1.4, 2026-05-11): METRICS_REPORT_REQ retired.
 * broker_proto 2 → 3 (audit C3, 2026-05-15): `role_uid` REQUIRED on
 *   DEREG_REQ + CONSUMER_DEREG_REQ wire payloads (HEP-CORE-0023 §2.1.1);
 *   CONSUMER_DIED_NOTIFY gains `consumer_uid` field (additive).
 * broker_proto 3 → 4 (audit B1, 2026-05-17): BAND_JOIN/LEAVE/BROADCAST/
 *   MEMBERS_REQ/ACK + BAND_*_NOTIFY wire payload key renamed
 *   `channel` → `band` per HEP-CORE-0030 §5.1.  Completes the
 *   2026-04-11 rename refactor (`8d3ee1e`) which renamed the C++
 *   surface and wire-message types but missed the payload key
 *   strings.  Old clients sending `channel` on a BAND_* request
 *   receive INVALID_REQUEST.
 * broker_proto 4 → 5 (audit R3.5b, 2026-05-19): wire-field
 *   unification — every role-context wire message now uses
 *   `role_uid`/`role_name`.  Renames: CONSUMER_REG_REQ.consumer_uid/
 *   consumer_name → role_uid/role_name; HEARTBEAT_REQ.uid →
 *   role_uid; BAND_BROADCAST_REQ.sender_uid → role_uid;
 *   CONSUMER_DIED_NOTIFY body.consumer_uid → role_uid.  Broker also
 *   enforces grammar (HEP-CORE-0033 §G2.2.0b) + side-aware role-tag
 *   policy at every gate (REG_REQ/DEREG_REQ accept tags {prod,proc};
 *   CONSUMER_REG_REQ/CONSUMER_DEREG_REQ accept {cons,proc};
 *   HEARTBEAT_REQ cross-checks role_type against tag).  Old clients
 *   using legacy keys get INVALID_REQUEST / INVALID_ROLE_TAG.
 *   Federation peer-context `sender_uid` (HUB_TARGETED_MSG /
 *   CHANNEL_BROADCAST_REQ from hubs) preserved (peer.uid, not
 *   role.uid).  Inbox-message `sender_uid` (PyInboxMsg /
 *   plh_inbox_msg_t / msg.sender_uid in Lua) preserved (authoring
 *   producer of an inbox payload — semantically distinct from local
 *   role.uid).
 * Keep in sync with `src/include/plh_version_registry.hpp` constants. */
#define PLH_COMPONENT_BROKER_PROTO_MAJOR   6
#define PLH_COMPONENT_BROKER_PROTO_MINOR   0
#define PLH_COMPONENT_ZMQ_FRAME_MAJOR      1
#define PLH_COMPONENT_ZMQ_FRAME_MINOR      0
#define PLH_COMPONENT_SCRIPT_API_MAJOR     1
#define PLH_COMPONENT_SCRIPT_API_MINOR     0
#define PLH_COMPONENT_SCRIPT_ENGINE_MAJOR  1
#define PLH_COMPONENT_SCRIPT_ENGINE_MINOR  2
#define PLH_COMPONENT_CONFIG_MAJOR         1
#define PLH_COMPONENT_CONFIG_MINOR         1

/* =========================================================================
 * Channel side constants (for spinlock, schema size queries)
 * ========================================================================= */

#define PLH_SIDE_TX  0   /**< Output / producer / Tx side */
#define PLH_SIDE_RX  1   /**< Input / consumer / Rx side */
#define PLH_SIDE_AUTO (-1) /**< Auto-select (single-side roles only; errors for processor) */

/* =========================================================================
 * Required symbols (implement these)
 * ========================================================================= */

/* bool native_init(const PlhNativeContext *ctx);  -- store ctx, return true */
/* void native_finalize(void);                    -- release resources */
/* const PlhAbiInfo *native_abi_info(void);        -- optional ABI check */

/* =========================================================================
 * Role callback symbols (implement the ones your role needs)
 * ========================================================================= */

/* Native callback ABI — three uniform shapes:
 *
 *   1. No-args lifecycle:           void name(void);
 *      on_init, on_stop, on_heartbeat
 *
 *   2. Structured-args lifecycle:   void name(const plh_X_args_t *);
 *      on_channel_closing, on_consumer_died
 *      See native_invoke_types.h for arg-struct definitions and the
 *      pointer-lifetime contract (valid for call duration only;
 *      plugin MUST NOT retain past return; strdup if a copy is needed).
 *
 *   3. Hot-path data:               bool name(const plh_X_t *);
 *      on_produce, on_consume, on_process, on_inbox
 *      Return value chooses commit (true) vs discard (false).
 *
 * Ad-hoc symbols invoked via the generic invoke(name [, args]) path
 * (NOT on the standard lifecycle list) use the legacy JSON-string
 * convention:
 *      void my_admin_thing(const char *args_json);
 *        - args_json is NULL when no arguments
 *        - args_json is a JSON object string when arguments are provided
 *        - string is valid for the duration of the call (stack-owned by host)
 *        - callee must copy if needed beyond return
 */
/* void on_init(void); */
/* void on_stop(void); */
/* void on_heartbeat(void); */
/* void on_channel_closing  (const plh_channel_closing_args_t   *args); */
/* void on_consumer_died    (const plh_consumer_died_args_t     *args); */
/* void on_hub_dead         (const plh_hub_dead_args_t          *args); */
/* void on_band_member_joined(const plh_band_member_joined_args_t *args); */
/* void on_band_member_left (const plh_band_member_left_args_t  *args); */
/* void on_band_message     (const plh_band_message_args_t      *args); */
/* void on_band_lost        (const plh_band_lost_args_t         *args); */
/* void on_allowlist_changed(const plh_allowlist_changed_args_t *args); */
/* bool on_produce (const plh_tx_t *tx); */
/* bool on_consume (const plh_rx_t *rx); */
/* bool on_process (const plh_rx_t *rx, const plh_tx_t *tx); */
/* bool on_inbox   (const plh_inbox_msg_t *msg); */

/* =========================================================================
 * Optional metadata symbols
 * ========================================================================= */

/* const char *native_name(void); */
/* const char *native_version(void); */
/* bool native_is_thread_safe(void); */

/* =========================================================================
 * Schema validation macros
 * ========================================================================= */

/**
 * @brief Declare schema descriptor for a slot type.
 * Generates native_schema_<Name>() and native_sizeof_<Name>() symbols.
 * The framework verifies the native engine's schema matches the JSON config.
 *
 * Note: The macro body includes extern "C" for C++ so that the generated
 * symbols have C linkage regardless of where the macro is expanded.
 */
#ifdef __cplusplus
#define PLH_DECLARE_SCHEMA(Name, Schema, Size)                                          \
    extern "C" PLH_EXPORT const char *native_schema_##Name(void) { return (Schema); }   \
    extern "C" PLH_EXPORT size_t native_sizeof_##Name(void) { return (Size); }
#else
#define PLH_DECLARE_SCHEMA(Name, Schema, Size)                              \
    PLH_EXPORT const char *native_schema_##Name(void) { return (Schema); }  \
    PLH_EXPORT size_t native_sizeof_##Name(void) { return (Size); }
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

/* =========================================================================
 * C++ convenience layer (optional — for C++ native engine authors)
 * ========================================================================= */

#ifdef __cplusplus

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

// std::format landed in C++20 but compiler support lagged (gcc 13+, clang
// 17+).  Feature-detect so older C++20 toolchains continue to build the
// rest of the wrapper.  Plugin authors on pre-format compilers can still
// call ctx.log(level, fmt::format(...).c_str()) explicitly.
#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#include <format>
#define PLH_HAS_STD_FORMAT 1
#else
#define PLH_HAS_STD_FORMAT 0
#endif

// Compile-time layout verification for cross-compiler interop.
static_assert(std::is_standard_layout_v<PlhNativeContext>,
              "PlhNativeContext must be standard-layout for C/C++ interop");
static_assert(alignof(PlhNativeContext) == 8,
              "PlhNativeContext must be 8-byte aligned");
static_assert(offsetof(PlhNativeContext, _magic) == 0,
              "PlhNativeContext: _magic must be at offset 0");

namespace plh
{

/** Log level enum for C++ usage. */
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

/* ── Typed enum classes (API v6 #194 Phase C) ─────────────────────────────
 *
 * Each enum derives from the C ABI macros at the top of this header.
 * Underlying type is `int` so `static_cast`-free interop with the C
 * fn ptrs is guaranteed.  See HEP-CORE-0028 §5.2. */

/** Negotiated CURVE mechanism for a queue side (HEP-CORE-0035 §2). */
enum class Mechanism : int
{
    Uninitialized = PLH_MECHANISM_UNINITIALIZED,
    Curve         = PLH_MECHANISM_CURVE,
};

/** Queue overflow / mode policy (HEP-CORE-0007 / HEP-CORE-0019). */
enum class QueuePolicy : int
{
    Unknown  = PLH_QUEUE_POLICY_UNKNOWN,
    Shm      = PLH_QUEUE_POLICY_SHM,
    ZmqDrop  = PLH_QUEUE_POLICY_ZMQ_DROP,
    ZmqBlock = PLH_QUEUE_POLICY_ZMQ_BLOCK,
    ZmqRing  = PLH_QUEUE_POLICY_ZMQ_RING,
};

/** Reason the role host was asked to stop (HEP-CORE-0001 +
 *  RoleHostCore::StopReason). */
enum class StopReason : int
{
    Normal        = PLH_STOP_REASON_NORMAL,
    PeerDead      = PLH_STOP_REASON_PEER_DEAD,
    HubDead       = PLH_STOP_REASON_HUB_DEAD,
    CriticalError = PLH_STOP_REASON_CRITICAL_ERROR,
    ChannelClosed = PLH_STOP_REASON_CHANNEL_CLOSED,
    ScriptError   = PLH_STOP_REASON_SCRIPT_ERROR,
};

/** Result of `Context::hub_post_event()`.  Tristate — the C ABI
 *  distinguishes "the event name is malformed" from "the broker /
 *  control loop is unhealthy"; the wrapper surfaces both so plugin
 *  authors can react appropriately (fix the name vs retry / log). */
enum class PostEventResult : int
{
    Accepted       = PLH_POST_EVENT_ACCEPTED,
    InvalidName    = PLH_POST_EVENT_INVALID_NAME,
    TransportError = PLH_POST_EVENT_TRANSPORT_ERROR,
};

/** Compile-time enum → string-view conversions for zero-alloc logging.
 *  Strings match the Python / Lua wire form so a Native plugin's log
 *  output reads identically to a Python plugin's. */
[[nodiscard]] constexpr std::string_view to_string(Mechanism m) noexcept
{
    switch (m)
    {
    case Mechanism::Curve:         return "Curve";
    case Mechanism::Uninitialized: return "Uninitialized";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view to_string(QueuePolicy p) noexcept
{
    switch (p)
    {
    case QueuePolicy::Shm:      return "shm";
    case QueuePolicy::ZmqDrop:  return "zmq_push_drop";
    case QueuePolicy::ZmqBlock: return "zmq_push_block";
    case QueuePolicy::ZmqRing:  return "zmq_pull_ring";
    case QueuePolicy::Unknown:  return "unknown";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(StopReason r) noexcept
{
    switch (r)
    {
    case StopReason::Normal:        return "normal";
    case StopReason::PeerDead:      return "peer_dead";
    case StopReason::HubDead:       return "hub_dead";
    case StopReason::CriticalError: return "critical_error";
    case StopReason::ChannelClosed: return "channel_closed";
    case StopReason::ScriptError:   return "script_error";
    }
    return "normal";
}

/* ── C++20 concepts for visitor type safety (#194 Phase C) ────────────────
 *
 * Concepts constrain visitor templates to callables of the right shape
 * AND enforce the HEP-CORE-0028 §4.8 "visitor MUST be noexcept" rule at
 * the type system, not just in docs.  A throwing visitor would otherwise
 * call std::terminate inside the noexcept thunk before the host's
 * try/catch can react.  Misuse → a clean compile error at the concept
 * boundary instead of a cryptic template diagnostic inside the thunk.
 * See HEP-CORE-0028 §5.3. */

template <typename V, typename Arg>
concept PlhNoexceptVisitorOf =
    std::invocable<V &, const Arg *>
 && std::is_nothrow_invocable_v<V &, const Arg *>;

template <typename V>
concept AllowedPeerVisitor = PlhNoexceptVisitorOf<V, plh_allowed_peer_t>;

template <typename V>
concept BandMemberVisitor  = PlhNoexceptVisitorOf<V, plh_band_member_t>;

// Forward declaration — Context is defined below; the handle types
// reference it via raw pointer.
class Context;

/**
 * @brief C++ wrapper around PlhNativeContext — borrowed-pointer view.
 *
 * Trivially copyable single-pointer wrapper.  All methods are inline and
 * dispatch through the context's function pointers — no host symbol
 * resolution.  The same Context value works on either side of the wire;
 * use `is_hub()` to branch hub-only from role-only paths (§4.9).
 *
 * Lifetime contract:
 *  - The Context wraps a borrowed `const PlhNativeContext *`.  The
 *    pointee is host-owned; valid from the start of native_init() until
 *    the return of native_finalize().  The plugin MUST NOT stash the
 *    raw pointer past native_finalize().
 *  - Identity strings (uid / name / channel / role_dir / log_level) are
 *    likewise borrowed and valid for the same lifetime.
 *  - Hub-side JSON returns (hub_*_json()) share a thread-local scratch
 *    buffer — valid only until the next hub_*_json() call on the same
 *    thread.  Copy via `std::string{ctx.hub_metrics_json()}` if you
 *    need a longer-lived value.
 *  - Visitor / event-args lifetime: see HEP-CORE-0028 §4.8.
 *
 * Usage:
 * ```cpp
 * static plh::Context *g_ctx;
 *
 * extern "C" bool native_init(const PlhNativeContext *raw) {
 *     static plh::Context ctx(raw);
 *     g_ctx = &ctx;
 *     ctx.log(plh::LogLevel::Info, "native engine initialized");
 *     if (ctx.is_hub())
 *         ctx.hub_post_event("plugin_started", R"({"version":"1.0"})");
 *     return true;
 * }
 * ```
 */
class Context
{
  public:
    explicit Context(const PlhNativeContext *c) noexcept : c_(c)
    {
        // Validate both sentinels at construction time.  If the magic
        // doesn't match, nullify the stored pointer — every accessor on
        // this Context becomes a safe no-op returning the documented
        // sentinel (nullptr / 0 / false / appropriate enum default).
        if (c && (c->_magic != PLH_CONTEXT_MAGIC || c->_magic_end != PLH_CONTEXT_MAGIC))
            c_ = nullptr;
    }

    /// Convenience overload — same magic check, accepts a reference.
    explicit Context(const PlhNativeContext &c) noexcept : Context(&c) {}

    // Context is a borrowed-pointer view (single pointer, trivially
    // copyable).  Pass freely by value; the copy is one register move.
    Context(const Context &)            noexcept = default;
    Context(Context &&)                 noexcept = default;
    Context &operator=(const Context &) noexcept = default;
    Context &operator=(Context &&)      noexcept = default;
    ~Context()                                   = default;

    /// Check if the context is valid (non-null + both magic sentinels match).
    [[nodiscard]] bool valid() const noexcept
    {
        return c_ != nullptr
            && c_->_magic     == PLH_CONTEXT_MAGIC
            && c_->_magic_end == PLH_CONTEXT_MAGIC;
    }

    // ── Identity ────────────────────────────────────────────────────
    [[nodiscard]] const char *uid()           const noexcept { return c_ ? c_->uid         : nullptr; }
    [[nodiscard]] const char *name()          const noexcept { return c_ ? c_->name        : nullptr; }
    [[nodiscard]] const char *channel()       const noexcept { return c_ ? c_->channel     : nullptr; }
    [[nodiscard]] const char *out_channel()   const noexcept { return c_ ? c_->out_channel : nullptr; }
    [[nodiscard]] const char *log_level_str() const noexcept { return c_ ? c_->log_level   : nullptr; }
    [[nodiscard]] const char *role_dir()      const noexcept { return c_ ? c_->role_dir    : nullptr; }
    [[nodiscard]] const char *role_tag()      const noexcept { return c_ ? c_->role_tag    : nullptr; }

    // ── Logging ─────────────────────────────────────────────────────
    void log(LogLevel level, const char *msg) const noexcept
    {
        if (c_ && c_->log) c_->log(c_, static_cast<int>(level), msg);
    }
    void log(LogLevel level, const std::string &msg) const noexcept
    {
        log(level, msg.c_str());
    }
    /// string_view overload — null-terminates into a small stack buffer.
    /// Falls back to a no-op if the view exceeds the buffer; document
    /// the cap to plugin authors so they choose between this and the
    /// const char * overload accordingly.
    void log(LogLevel level, std::string_view msg) const noexcept
    {
        if (!c_ || !c_->log) return;
        char buf[1024];
        if (msg.size() >= sizeof(buf)) return;          // log message dropped
        std::memcpy(buf, msg.data(), msg.size());
        buf[msg.size()] = '\0';
        c_->log(c_, static_cast<int>(level), buf);
    }

#if PLH_HAS_STD_FORMAT
    /// std::format overload — type-safe inline formatting.  Available
    /// when the toolchain ships `<format>` (gcc 13+, clang 17+, MSVC
    /// 19.29+).  Pre-format C++20 toolchains skip the overload; plugins
    /// must explicitly pre-format and pass `std::string{...}.c_str()`.
    ///
    /// Allocates a `std::string` (vformat_to a string) — log_string_view
    /// path is preferred when no formatting is needed.  No silent
    /// truncation: messages of any length route through.
    template <typename... Args>
    void log(LogLevel level,
             std::format_string<Args...> fmt,
             Args &&...args) const noexcept
    {
        if (!c_ || !c_->log) return;
        try
        {
            const std::string msg =
                std::vformat(fmt.get(), std::make_format_args(args...));
            c_->log(c_, static_cast<int>(level), msg.c_str());
        }
        catch (...)
        {
            // std::format throws on argument count/type mismatch only at
            // runtime via std::format_error — should not happen with
            // format_string<Args...>'s compile-time check, but
            // defensively swallow to honour noexcept.
        }
    }
#endif

    // ── Custom metrics ──────────────────────────────────────────────
    void report_metric(const char *key, double value) const noexcept
    {
        if (c_ && c_->report_metric) c_->report_metric(c_, key, value);
    }
    void clear_custom_metrics() const noexcept
    {
        if (c_ && c_->clear_custom_metrics) c_->clear_custom_metrics(c_);
    }

    // ── Lifecycle ───────────────────────────────────────────────────
    void request_stop() const noexcept
    {
        if (c_ && c_->request_stop) c_->request_stop(c_);
    }
    /// Flag a critical error and request shutdown.  msg is REQUIRED:
    /// a null-terminated string describing the unrecoverable
    /// condition.  Logged by the host at ERROR level as
    /// `[role_tag/uid] CRITICAL: <msg>` BEFORE flipping state.
    /// Uniform with Python `api.set_critical_error(msg)` and Lua
    /// `api.set_critical_error(msg)`.
    void set_critical_error(const char *msg) const noexcept
    {
        if (c_ && c_->set_critical_error) c_->set_critical_error(c_, msg);
    }
    [[nodiscard]] bool is_critical_error() const noexcept
    {
        return c_ && c_->is_critical_error && c_->is_critical_error(c_) != 0;
    }
    /// API v6 — returns a typed StopReason enum class.  See HEP-CORE-0028 §5.2.
    [[nodiscard]] StopReason stop_reason() const noexcept
    {
        return (c_ && c_->stop_reason)
            ? static_cast<StopReason>(c_->stop_reason(c_))
            : StopReason::Normal;
    }

    // ── Counters ────────────────────────────────────────────────────
    [[nodiscard]] uint64_t out_slots_written()  const noexcept { return (c_ && c_->out_slots_written)  ? c_->out_slots_written(c_)  : 0; }
    [[nodiscard]] uint64_t in_slots_received()  const noexcept { return (c_ && c_->in_slots_received)  ? c_->in_slots_received(c_)  : 0; }
    [[nodiscard]] uint64_t out_drop_count()     const noexcept { return (c_ && c_->out_drop_count)     ? c_->out_drop_count(c_)     : 0; }
    [[nodiscard]] uint64_t script_error_count() const noexcept { return (c_ && c_->script_error_count) ? c_->script_error_count(c_) : 0; }
    [[nodiscard]] uint64_t loop_overrun_count() const noexcept { return (c_ && c_->loop_overrun_count) ? c_->loop_overrun_count(c_) : 0; }
    [[nodiscard]] uint64_t last_cycle_work_us() const noexcept { return (c_ && c_->last_cycle_work_us) ? c_->last_cycle_work_us(c_) : 0; }

    // ── Spinlock ───────────────────────────────────────────────────
    [[nodiscard]] bool spinlock_lock(int index, int side = PLH_SIDE_AUTO, int timeout_ms = -1) const noexcept
    {
        return c_ && c_->spinlock_lock && c_->spinlock_lock(c_, index, side, timeout_ms) != 0;
    }
    void spinlock_unlock(int index, int side = PLH_SIDE_AUTO) const noexcept
    {
        if (c_ && c_->spinlock_unlock) c_->spinlock_unlock(c_, index, side);
    }
    [[nodiscard]] uint32_t spinlock_count(int side = PLH_SIDE_AUTO) const noexcept
    {
        return (c_ && c_->spinlock_count) ? c_->spinlock_count(c_, side) : 0;
    }
    [[nodiscard]] bool spinlock_is_locked(int index, int side = PLH_SIDE_AUTO) const noexcept
    {
        return c_ && c_->spinlock_is_locked && c_->spinlock_is_locked(c_, index, side) != 0;
    }

    /// RAII spinlock guard — releases on scope exit.
    class SpinLockGuard
    {
      public:
        SpinLockGuard(const Context &api, int index, int side = PLH_SIDE_AUTO,
                      int timeout_ms = -1)
            : api_(&api), index_(index), side_(side)
            , locked_(api.spinlock_lock(index, side, timeout_ms))
        {}
        ~SpinLockGuard() { if (locked_) api_->spinlock_unlock(index_, side_); }

        SpinLockGuard(const SpinLockGuard &) = delete;
        SpinLockGuard &operator=(const SpinLockGuard &) = delete;

        bool owns_lock() const noexcept { return locked_; }
        explicit operator bool() const noexcept { return locked_; }
        int index() const noexcept { return index_; }
        int side() const noexcept { return side_; }

      private:
        const Context *api_;
        int index_;
        int side_;
        bool locked_;
    };

    /// Execute fn while holding spinlock acquired by index+side.
    /// Returns false if lock acquisition fails.
    /// Exception-safe: RAII guard releases the lock during stack unwinding
    /// if fn throws — the exception propagates to the caller after release.
    /// NOT noexcept — fn is free to throw.
    ///
    /// Note: fn's return value (if any) is intentionally discarded.  The
    /// `bool` return communicates lock-acquisition status only.  Capture
    /// data via fn's closure if you need to surface a value:
    ///   `int value = 0; api.with_spinlock(0, ..., [&]{ value = compute(); });`
    template <typename Fn>
    bool with_spinlock(int index, int side, int timeout_ms, Fn &&fn) const
    {
        SpinLockGuard guard(*this, index, side, timeout_ms);
        if (!guard) return false;
        fn();
        return true;
    }

    /// Execute fn while holding an additional recursive lock on an existing guard.
    /// Safe even if guard already holds the lock (recursive locking increments
    /// the recursion counter; the inner guard's destructor decrements it).
    /// Returns false if lock acquisition fails.
    template <typename Fn>
    bool with_spinlock(SpinLockGuard &existing, int timeout_ms, Fn &&fn) const
    {
        // Re-lock the same index+side — recursive, safe on same thread.
        SpinLockGuard inner(*this, existing.index(), existing.side(), timeout_ms);
        if (!inner) return false;
        fn();
        return true;
    }

    // ── Schema sizes ───────────────────────────────────────────────
    [[nodiscard]] size_t slot_logical_size(int side = PLH_SIDE_AUTO) const noexcept
    {
        return (c_ && c_->slot_logical_size) ? c_->slot_logical_size(c_, side) : 0;
    }
    [[nodiscard]] size_t flexzone_logical_size(int side = PLH_SIDE_AUTO) const noexcept
    {
        return (c_ && c_->flexzone_logical_size) ? c_->flexzone_logical_size(c_, side) : 0;
    }

    // Flexzone: C/C++ plugins access fz directly via plh_tx_t.fz / plh_rx_t.fz
    // in the invoke callback — zero-cost, no function pointer dispatch needed.
    // No separate flexzone() accessor on Context; the invoke struct IS the access
    // path for C/C++. Python/Lua use api.flexzone(side) because reconstructing
    // typed views per invoke is expensive in those languages.

    [[nodiscard]] bool wait_for_role(const char *uid, int timeout_ms = 5000) const noexcept
    {
        return c_ && c_->wait_for_role && c_->wait_for_role(c_, uid, timeout_ms) != 0;
    }

    // ── Band pub/sub (HEP-CORE-0030) — API v6 ──────────────────────
    /// Join a band by name.  Returns true on success, false on rejection
    /// or transport error.  API v6 drops the list-from-join behaviour —
    /// call `band(channel).visit_members(...)` separately to enumerate.
    [[nodiscard]] bool band_join(const char *channel) const noexcept
    {
        return c_ && c_->band_join && c_->band_join(c_, channel) == 1;
    }
    [[nodiscard]] bool band_leave(const char *channel) const noexcept
    {
        return c_ && c_->band_leave && c_->band_leave(c_, channel) == 1;
    }
    void band_broadcast(const char *channel, const char *body_json) const noexcept
    {
        if (c_ && c_->band_broadcast) c_->band_broadcast(c_, channel, body_json);
    }

    /// Returns a lightweight handle for band-membership queries.  Zero
    /// allocation — see HEP-CORE-0028 §5.5.
    [[nodiscard]] constexpr inline class BandHandle band(const char *channel) const noexcept;

    // ── Channel-auth observability (HEP-CORE-0036 §I11 + §6.7) ──────
    /// Returns a handle for authorized-peer queries on `channel`.
    /// See HEP-CORE-0028 §5.5.
    [[nodiscard]] constexpr inline class AllowedPeersHandle
    allowed_peers(const char *channel) const noexcept;

    /// True iff the named channel's queue is Active (§6.7).
    [[nodiscard]] bool is_channel_ready(const char *channel) const noexcept
    {
        return c_ && c_->is_channel_ready
            && c_->is_channel_ready(c_, channel) == 1;
    }

    /// Returns the negotiated CURVE mechanism for the named side as a
    /// typed enum class.  API v6 — replaces v5 const char * return.
    [[nodiscard]] Mechanism queue_mechanism(int side = PLH_SIDE_AUTO) const noexcept
    {
        return (c_ && c_->queue_mechanism)
            ? static_cast<Mechanism>(c_->queue_mechanism(c_, side))
            : Mechanism::Uninitialized;
    }

    // ── Metrics snapshot (#194 Phase C) ─────────────────────────────
    /// Build a thread-local snapshot of the current metrics tree and
    /// return a typed view.  Lifetime: valid until next snapshot on
    /// same thread.  See HEP-CORE-0028 §5.4.
    [[nodiscard]] inline class MetricsSnapshot metrics_snapshot() const noexcept;

    // ── Flexzone control + band-membership query ───────────────────
    /// True iff this role is a current member of the band routing.
    [[nodiscard]] bool is_in_band(const char *channel) const noexcept
    {
        return c_ && c_->is_in_band && c_->is_in_band(c_, channel) == 1;
    }
    /// SHM-only.  Recompute + store flexzone checksum.
    [[nodiscard]] bool update_flexzone_checksum() const noexcept
    {
        return c_ && c_->update_flexzone_checksum
            && c_->update_flexzone_checksum(c_) == 1;
    }
    /// SHM-only.  Enable/disable per-read checksum verification.
    void set_verify_checksum(bool enable) const noexcept
    {
        if (c_ && c_->set_verify_checksum)
            c_->set_verify_checksum(c_, enable ? 1 : 0);
    }

    // ── Queue diagnostics — depth + policy + last_seq ──────────────
    [[nodiscard]] uint64_t out_capacity() const noexcept
    {
        return (c_ && c_->out_capacity) ? c_->out_capacity(c_) : 0;
    }
    [[nodiscard]] uint64_t in_capacity() const noexcept
    {
        return (c_ && c_->in_capacity) ? c_->in_capacity(c_) : 0;
    }
    /// Output queue overflow policy as a typed enum.  API v6 — replaces
    /// v5 malloc'd char * return.
    [[nodiscard]] QueuePolicy out_policy() const noexcept
    {
        return (c_ && c_->out_policy)
            ? static_cast<QueuePolicy>(c_->out_policy(c_))
            : QueuePolicy::Unknown;
    }
    [[nodiscard]] QueuePolicy in_policy() const noexcept
    {
        return (c_ && c_->in_policy)
            ? static_cast<QueuePolicy>(c_->in_policy(c_))
            : QueuePolicy::Unknown;
    }
    [[nodiscard]] uint64_t last_seq() const noexcept
    {
        return (c_ && c_->last_seq) ? c_->last_seq(c_) : 0;
    }

    // ── Hub-side surface (API v7, #84 2026-06-11) ───────────────────
    //
    // Convenience wrappers around the hub_* fn ptrs.  All return an
    // empty/sentinel value on a role-side context (where the wire layer
    // routes to role_stub_hub_*) so plugin code can be written once
    // and dispatch by `is_hub()` rather than null-checking every call.
    //
    // String returns: empty `""` for absent/unset.  JSON returns are
    // owned by a thread-local scratch buffer in the host — valid until
    // the next hub_* call on the same thread (HEP-CORE-0028 §4.9).

    /// Is this Context attached to a hub (vs a role)?  Detected via
    /// `role_tag == "hub"` — the literal string the host writes during
    /// `wire_hub()`.  Other framework role_tag values today:
    /// `"producer"`, `"consumer"`, `"processor"`.  Future federation
    /// peer designs may add new role_tag values; if so this predicate
    /// would need updating in lock-step.
    [[nodiscard]] bool is_hub() const noexcept
    {
        return c_ && c_->role_tag && std::string_view{c_->role_tag} == "hub";
    }

    [[nodiscard]] const char *hub_metrics_json() const noexcept
    {
        return (c_ && c_->hub_metrics_json) ? c_->hub_metrics_json(c_) : "";
    }
    [[nodiscard]] const char *hub_config_json() const noexcept
    {
        return (c_ && c_->hub_config_json) ? c_->hub_config_json(c_) : "";
    }
    [[nodiscard]] const char *hub_query_metrics_json(const char *categories_json) const noexcept
    {
        return (c_ && c_->hub_query_metrics_json)
            ? c_->hub_query_metrics_json(c_, categories_json) : "";
    }
    [[nodiscard]] const char *hub_list_channels_json() const noexcept
    {
        return (c_ && c_->hub_list_channels_json) ? c_->hub_list_channels_json(c_) : "";
    }
    [[nodiscard]] const char *hub_get_channel_json(const char *name) const noexcept
    {
        return (c_ && c_->hub_get_channel_json) ? c_->hub_get_channel_json(c_, name) : "";
    }
    [[nodiscard]] const char *hub_list_roles_json() const noexcept
    {
        return (c_ && c_->hub_list_roles_json) ? c_->hub_list_roles_json(c_) : "";
    }
    [[nodiscard]] const char *hub_get_role_json(const char *uid) const noexcept
    {
        return (c_ && c_->hub_get_role_json) ? c_->hub_get_role_json(c_, uid) : "";
    }
    [[nodiscard]] const char *hub_list_bands_json() const noexcept
    {
        return (c_ && c_->hub_list_bands_json) ? c_->hub_list_bands_json(c_) : "";
    }
    [[nodiscard]] const char *hub_get_band_json(const char *name) const noexcept
    {
        return (c_ && c_->hub_get_band_json) ? c_->hub_get_band_json(c_, name) : "";
    }
    [[nodiscard]] const char *hub_list_peers_json() const noexcept
    {
        return (c_ && c_->hub_list_peers_json) ? c_->hub_list_peers_json(c_) : "";
    }
    [[nodiscard]] const char *hub_get_peer_json(const char *hub_uid) const noexcept
    {
        return (c_ && c_->hub_get_peer_json) ? c_->hub_get_peer_json(c_, hub_uid) : "";
    }

    /// Schedule the close of a channel.  Returns true on accept.
    [[nodiscard]] bool hub_close_channel(const char *name) const noexcept
    {
        return c_ && c_->hub_close_channel && c_->hub_close_channel(c_, name) == 1;
    }
    /// Send a control-plane broadcast.  data_json may be null/empty.
    [[nodiscard]] bool hub_broadcast_channel(const char *channel,
                                              const char *message,
                                              const char *data_json) const noexcept
    {
        return c_ && c_->hub_broadcast_channel
            && c_->hub_broadcast_channel(c_, channel, message, data_json) == 1;
    }
    /// Post a user-defined event.  Returns a tristate (§4.9):
    ///  - `Accepted`       — event queued; `on_app_<name>` will fire.
    ///  - `InvalidName`    — name failed HEP-CORE-0033 G2.2.0b identifier
    ///                       check; nothing posted.  Plugin bug.
    ///  - `TransportError` — control loop unhealthy / fn ptr unwired.
    ///                       Treat as observational; consider retry.
    /// data_json may be null.
    [[nodiscard]] PostEventResult hub_post_event(const char *name,
                                                  const char *data_json) const noexcept
    {
        if (!c_ || !c_->hub_post_event) return PostEventResult::TransportError;
        return static_cast<PostEventResult>(c_->hub_post_event(c_, name, data_json));
    }

    [[nodiscard]] int64_t hub_augment_timeout_ms() const noexcept
    {
        return (c_ && c_->hub_augment_timeout_ms) ? c_->hub_augment_timeout_ms(c_) : 0;
    }
    void hub_set_augment_timeout(int64_t ms) const noexcept
    {
        if (c_ && c_->hub_set_augment_timeout) c_->hub_set_augment_timeout(c_, ms);
    }

    /// Access the raw C context.  May be nullptr if the Context was
    /// constructed from an invalid PlhNativeContext (magic mismatch).
    [[nodiscard]] constexpr const PlhNativeContext *raw() const noexcept { return c_; }

  private:
    const PlhNativeContext *c_;
};

/* ── MetricsSnapshot — opaque handle wrapper (#194 Phase C) ──────────────
 *
 * Typed view over the opaque pointer returned by
 * `ctx->metrics_snapshot()`.  NOT a RAII resource — the underlying
 * thread-local cache is host-owned; this wrapper merely binds the
 * pointer to a typed lookup interface.  Lifetime contract: valid until
 * the next `metrics_snapshot()` call on the SAME thread.  See
 * HEP-CORE-0028 §5.4. */
class MetricsSnapshot
{
  public:
    constexpr MetricsSnapshot(const Context &ctx, const void *snap) noexcept
        : ctx_(&ctx), snap_(snap) {}

    /// Get a metric by dotted-path key.  Returns nullopt if missing,
    /// snapshot invalid, or ABI fn unwired.
    [[nodiscard]] std::optional<double> get(const char *key) const noexcept
    {
        if (!snap_ || !ctx_->raw()->metrics_get || !key) return std::nullopt;
        double v = 0.0;
        if (ctx_->raw()->metrics_get(snap_, key, &v) == 1) return v;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<double> get(std::string_view key) const noexcept
    {
        // string_view may not be null-terminated; copy to a stack buffer
        // first.  Buffer size 512 chosen to cover HEP-CORE-0019 §9
        // dotted-path metric keys with comfortable headroom (deepest
        // observed tree path < 128 chars).  Over-cap returns nullopt
        // (silent-vs-explicit trade-off: plugin authors using
        // string_view that long should use the const char* overload
        // with an explicit owned buffer).
        if (key.size() >= 512) return std::nullopt;
        char buf[512];
        std::memcpy(buf, key.data(), key.size());
        buf[key.size()] = '\0';
        return get(buf);
    }

    /// Ergonomic shorthand for get().
    [[nodiscard]] std::optional<double> operator[](const char *key) const noexcept
    { return get(key); }

    [[nodiscard]] explicit operator bool() const noexcept { return snap_ != nullptr; }
    [[nodiscard]] constexpr const void *raw() const noexcept { return snap_; }

  private:
    const Context *ctx_;
    const void    *snap_;
};

/* ── AllowedPeersHandle — handle for HEP-0036 §I11 surface ───────────────
 *
 * Lightweight typed view over (ctx, channel).  Two pointers, zero
 * allocation.  Methods delegate to the C ABI fn ptrs with no extra
 * runtime work.  See HEP-CORE-0028 §5.5. */
class AllowedPeersHandle
{
  public:
    constexpr AllowedPeersHandle(const Context &ctx, const char *channel) noexcept
        : ctx_(&ctx), channel_(channel) {}

    /// Template visitor — concept-constrained, zero-cost thunk forwards
    /// to the C ABI.  Visitor MUST be noexcept; throwing across the C
    /// ABI boundary is undefined behaviour (HEP-CORE-0028 §4.8).
    template <AllowedPeerVisitor V>
    int visit(V &&v) const noexcept
    {
        const auto *c = ctx_->raw();
        if (!c->allowed_peers) return -1;
        auto thunk = +[](const plh_allowed_peer_t *p, void *ud) noexcept {
            (*static_cast<std::decay_t<V> *>(ud))(p);
        };
        return c->allowed_peers(c, channel_, thunk, &v);
    }

    /// Raw-fnptr overload — for callers that already have a plain
    /// function pointer.
    int visit(plh_allowed_peer_visitor v, void *userdata) const noexcept
    {
        const auto *c = ctx_->raw();
        return c->allowed_peers
            ? c->allowed_peers(c, channel_, v, userdata)
            : -1;
    }

    /// 1 if `role_uid` is in the channel's allowlist, 0 if not, -1 on
    /// error.  Wrapper presents as bool — use count() if you need to
    /// distinguish missing from error.
    [[nodiscard]] bool contains(const char *role_uid) const noexcept
    {
        const auto *c = ctx_->raw();
        return c->allowed_peer_contains
            ? c->allowed_peer_contains(c, channel_, role_uid) == 1
            : false;
    }

    /// Authorized-peer count (>=0) or -1 on error.
    [[nodiscard]] int count() const noexcept
    {
        const auto *c = ctx_->raw();
        return c->allowed_peer_count ? c->allowed_peer_count(c, channel_) : -1;
    }

    /// Snapshot the allowlist's role UIDs into a std::vector.  Encourages
    /// the "snapshot once, query many times" pattern for hot paths
    /// (HEP-CORE-0028 §5.5).  Lambda is marked noexcept to satisfy the
    /// PlhNoexceptVisitorOf concept; `std::bad_alloc` propagates as
    /// `std::terminate` (standard C++ behavior on container allocator
    /// failure — there is no recovery from OOM in data-path code).
    [[nodiscard]] std::vector<std::string> collect_uids() const
    {
        std::vector<std::string> out;
        visit([&](const plh_allowed_peer_t *p) noexcept {
            if (p && p->role_uid) out.emplace_back(p->role_uid);
        });
        return out;
    }

    /// Snapshot into an unordered_set for O(1) repeated membership
    /// queries.  Use when the plugin queries the same allowlist many
    /// times per cycle — measurably faster than repeated contains()
    /// calls (each of which pays a per-call vector copy in the host).
    [[nodiscard]] std::unordered_set<std::string> to_uid_set() const
    {
        std::unordered_set<std::string> out;
        visit([&](const plh_allowed_peer_t *p) noexcept {
            if (p && p->role_uid) out.emplace(p->role_uid);
        });
        return out;
    }

    [[nodiscard]] constexpr const char *channel() const noexcept { return channel_; }

  private:
    const Context *ctx_;
    const char    *channel_;
};

/* ── BandHandle — handle for HEP-CORE-0030 §5.3 surface ─────────────────
 *
 * Same shape as AllowedPeersHandle — visitor + contains + count
 * triplet, plus broadcast pass-through.  Surface uniformity: plugin
 * authors who learn AllowedPeersHandle already know BandHandle.  See
 * HEP-CORE-0028 §5.5. */
class BandHandle
{
  public:
    constexpr BandHandle(const Context &ctx, const char *channel) noexcept
        : ctx_(&ctx), channel_(channel) {}

    template <BandMemberVisitor V>
    int visit_members(V &&v) const noexcept
    {
        const auto *c = ctx_->raw();
        if (!c->band_members) return -1;
        auto thunk = +[](const plh_band_member_t *m, void *ud) noexcept {
            (*static_cast<std::decay_t<V> *>(ud))(m);
        };
        return c->band_members(c, channel_, thunk, &v);
    }

    int visit_members(plh_band_member_visitor v, void *userdata) const noexcept
    {
        const auto *c = ctx_->raw();
        return c->band_members
            ? c->band_members(c, channel_, v, userdata)
            : -1;
    }

    [[nodiscard]] bool contains(const char *role_uid) const noexcept
    {
        const auto *c = ctx_->raw();
        return c->band_member_contains
            ? c->band_member_contains(c, channel_, role_uid) == 1
            : false;
    }

    [[nodiscard]] int member_count() const noexcept
    {
        const auto *c = ctx_->raw();
        return c->band_member_count ? c->band_member_count(c, channel_) : -1;
    }

    void broadcast(const char *body_json) const noexcept
    {
        const auto *c = ctx_->raw();
        if (c->band_broadcast) c->band_broadcast(c, channel_, body_json);
    }

    [[nodiscard]] std::vector<std::string> collect_uids() const
    {
        std::vector<std::string> out;
        visit_members([&](const plh_band_member_t *m) noexcept {
            if (m && m->role_uid) out.emplace_back(m->role_uid);
        });
        return out;
    }

    [[nodiscard]] std::unordered_set<std::string> to_uid_set() const
    {
        std::unordered_set<std::string> out;
        visit_members([&](const plh_band_member_t *m) noexcept {
            if (m && m->role_uid) out.emplace(m->role_uid);
        });
        return out;
    }

    [[nodiscard]] constexpr const char *channel() const noexcept { return channel_; }

  private:
    const Context *ctx_;
    const char    *channel_;
};

/* ── AllowlistChangedView — std::span wrapper over event args ────────────
 *
 * Wraps `plh_allowlist_changed_args_t *` to expose the peers array as
 * a `std::span` for idiomatic range-for iteration.  See HEP-CORE-0028
 * §5.6. */
class AllowlistChangedView
{
  public:
    constexpr explicit AllowlistChangedView(
        const plh_allowlist_changed_args_t *a) noexcept
        : args_(a) {}

    [[nodiscard]] constexpr std::string_view channel() const noexcept
    { return args_ && args_->channel ? args_->channel : std::string_view{}; }

    [[nodiscard]] constexpr std::string_view reason() const noexcept
    { return args_ && args_->reason ? args_->reason : std::string_view{}; }

    [[nodiscard]] constexpr std::span<const plh_allowed_peer_t> peers() const noexcept
    {
        return args_ ? std::span{args_->peers, args_->peer_count}
                     : std::span<const plh_allowed_peer_t>{};
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    { return args_ != nullptr; }

  private:
    const plh_allowlist_changed_args_t *args_;
};

/* ── Context inline factory methods (defined after handle types) ──────── */

constexpr inline AllowedPeersHandle Context::allowed_peers(const char *channel) const noexcept
{
    return AllowedPeersHandle(*this, channel);
}

constexpr inline BandHandle Context::band(const char *channel) const noexcept
{
    return BandHandle(*this, channel);
}

inline MetricsSnapshot Context::metrics_snapshot() const noexcept
{
    return MetricsSnapshot(
        *this,
        (c_ && c_->metrics_snapshot) ? c_->metrics_snapshot(c_) : nullptr);
}

/* ── Compile-time verification of zero-cost handle contract (#194 Phase C) ─
 *
 * The contract: handles are trivially-copyable two-pointer values that
 * pass in registers.  If a future change adds a non-trivial member,
 * build breaks here. */

static_assert(std::is_trivially_copyable_v<Context>,
              "plh::Context must be trivially copyable (single borrowed pointer)");
static_assert(sizeof(Context) == sizeof(void *),
              "plh::Context must be exactly one pointer");
static_assert(std::is_trivially_copyable_v<MetricsSnapshot>,
              "plh::MetricsSnapshot must be trivially copyable");
static_assert(std::is_trivially_copyable_v<AllowedPeersHandle>,
              "plh::AllowedPeersHandle must be trivially copyable");
static_assert(std::is_trivially_copyable_v<BandHandle>,
              "plh::BandHandle must be trivially copyable");
static_assert(std::is_trivially_copyable_v<AllowlistChangedView>,
              "plh::AllowlistChangedView must be trivially copyable");
static_assert(sizeof(AllowedPeersHandle) == 2 * sizeof(void *),
              "plh::AllowedPeersHandle must be exactly two pointers");
static_assert(sizeof(BandHandle) == 2 * sizeof(void *),
              "plh::BandHandle must be exactly two pointers");

/**
 * @brief Typed slot reference — zero-cost wrapper around raw pointer.
 */
template <typename T>
class SlotRef
{
    static_assert(std::is_standard_layout_v<T>,
                  "SlotRef<T>: T must be a standard-layout type");
  public:
    SlotRef(void *ptr, size_t sz) noexcept
        : ptr_(static_cast<T *>(ptr)), valid_(ptr != nullptr && sz >= sizeof(T))
    {}

    explicit operator bool() const noexcept { return valid_; }
    T *operator->() noexcept { return ptr_; }
    const T *operator->() const noexcept { return ptr_; }
    T &operator*() noexcept { return *ptr_; }
    const T &operator*() const noexcept { return *ptr_; }
    T *get() noexcept { return ptr_; }
    const T *get() const noexcept { return ptr_; }

  private:
    T   *ptr_;
    bool valid_;
};

/// Read-only variant.
template <typename T>
class ConstSlotRef
{
    static_assert(std::is_standard_layout_v<T>,
                  "ConstSlotRef<T>: T must be a standard-layout type");
  public:
    ConstSlotRef(const void *ptr, size_t sz) noexcept
        : ptr_(static_cast<const T *>(ptr)), valid_(ptr != nullptr && sz >= sizeof(T))
    {}

    explicit operator bool() const noexcept { return valid_; }
    const T *operator->() const noexcept { return ptr_; }
    const T &operator*() const noexcept { return *ptr_; }
    const T *get() const noexcept { return ptr_; }

  private:
    const T *ptr_;
    bool     valid_;
};

} // namespace plh

/* ── Export macros for C++ typed callbacks ──────────────────────────────── */

#define PLH_EXPORT_PRODUCE(SlotType, FlexType, func)                        \
    extern "C" PLH_EXPORT bool on_produce(const plh_tx_t *tx)               \
    { return func(plh::SlotRef<SlotType>(tx->slot, tx->slot_size),          \
                  plh::SlotRef<FlexType>(tx->fz, tx->fz_size)); }

#define PLH_EXPORT_PRODUCE_NOFZ(SlotType, func)                             \
    extern "C" PLH_EXPORT bool on_produce(const plh_tx_t *tx)               \
    { return func(plh::SlotRef<SlotType>(tx->slot, tx->slot_size)); }

#define PLH_EXPORT_CONSUME(SlotType, FlexType, func)                        \
    extern "C" PLH_EXPORT bool on_consume(const plh_rx_t *rx)               \
    { return func(plh::ConstSlotRef<SlotType>(rx->slot, rx->slot_size),     \
                  plh::SlotRef<FlexType>(rx->fz, rx->fz_size)); }

#define PLH_EXPORT_CONSUME_NOFZ(SlotType, func)                             \
    extern "C" PLH_EXPORT bool on_consume(const plh_rx_t *rx)               \
    { return func(plh::ConstSlotRef<SlotType>(rx->slot, rx->slot_size)); }

#define PLH_EXPORT_PROCESS(InType, OutType, FlexType, func)                 \
    extern "C" PLH_EXPORT bool on_process(                                  \
        const plh_rx_t *rx, const plh_tx_t *tx)                             \
    { return func(plh::ConstSlotRef<InType>(rx->slot, rx->slot_size),       \
                  plh::SlotRef<OutType>(tx->slot, tx->slot_size),           \
                  plh::SlotRef<FlexType>(tx->fz, tx->fz_size)); }

#define PLH_EXPORT_PROCESS_NOFZ(InType, OutType, func)                      \
    extern "C" PLH_EXPORT bool on_process(                                  \
        const plh_rx_t *rx, const plh_tx_t *tx)                             \
    { return func(plh::ConstSlotRef<InType>(rx->slot, rx->slot_size),       \
                  plh::SlotRef<OutType>(tx->slot, tx->slot_size)); }

#endif /* __cplusplus */

#endif /* PYLABHUB_NATIVE_ENGINE_API_H */
