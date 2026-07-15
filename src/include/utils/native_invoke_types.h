/**
 * @file native_invoke_types.h
 * @brief C-ABI invoke direction structs for native engine callbacks.
 *
 * Defines plh_rx_t (input), plh_tx_t (output), and plh_inbox_msg_t (inbox).
 * Pure C header — no C++ dependencies. Included by native_engine_api.h
 * and native_engine.hpp.
 *
 * Slot is const on the rx (input) side, writable on the tx (output) side.
 * Flexzone is always writable on both sides (HEP-0002 TABLE 1).
 */

#ifndef PYLABHUB_NATIVE_INVOKE_TYPES_H
#define PYLABHUB_NATIVE_INVOKE_TYPES_H

#include <stddef.h>
#include <stdint.h>

/** Loop-ready status returned by `on_init` — HEP-CORE-0011 §"Loop-ready
 *  gate".  Native ABI v9 changed `on_init` from `void`-return to this
 *  status.  Values chosen so that PLH_INIT_NOT_READY=0 matches the C
 *  convention of "0 = false / not-ready" and PLH_INIT_READY=1 matches
 *  "1 = true / ready", letting plugin code use `return true;` idiomatically.
 *  Defined here (not in native_engine_api.h) so both plugin authors
 *  (via that header) AND the host-side engine wiring (native_engine.hpp)
 *  see the same typedef without either including the other.
 *  Symbolic PLH_INIT_* macros live in native_engine_api.h. */
typedef int plh_init_status_t;

/** Input direction — data received from upstream.
 *  fz/fz_size are populated by the native engine bridge at invoke time
 *  from the init-time cache — callers must NOT set these manually. */
typedef struct
{
    const void *slot;       /**< Read-only input slot. */
    size_t      slot_size;
    void       *fz;         /**< Mutable flexzone — bridge-populated from cached api pointer. */
    size_t      fz_size;
} plh_rx_t;

/** Output direction — data going downstream.
 *  fz/fz_size are populated by the native engine bridge at invoke time. */
typedef struct
{
    void  *slot;            /**< Writable output slot. */
    size_t slot_size;
    void  *fz;              /**< Mutable flexzone — bridge-populated from cached api pointer. */
    size_t fz_size;
} plh_tx_t;

/** Inbox message — one-shot peer-to-peer delivery. */
typedef struct
{
    const void *data;       /**< Typed payload (valid until next recv). */
    size_t      data_size;
    const char *sender_uid; /**< Sender's role UID. */
    uint64_t    seq;        /**< Sender's monotonic sequence number. */
} plh_inbox_msg_t;

/* ============================================================================
 * Lifecycle callback argument structs.
 *
 * All `const char *` fields are valid ONLY for the duration of the
 * callback (the host holds the backing storage in its stack frame
 * across the call).  Plugins MUST NOT free these pointers; MUST NOT
 * retain them past return.  If a copy is needed, use `strdup` and
 * own the copy.
 *
 * ABI versioning: fields may be APPENDED at the end of these structs
 * without breaking plugins compiled against an older definition.
 * Field types and existing field order are stable.  Do not reorder,
 * remove, or change types of existing fields.
 * ========================================================================== */

/** on_channel_closing args (HEP-CORE-0011; HEP-CORE-0023 §2.1.1).
 *  Fired when the broker closes a channel the role is registered on
 *  (last-producer drop, script-requested close, etc.). */
typedef struct
{
    const char *channel;        /**< Closed channel name. */
    const char *reason;         /**< e.g. "producer_deregistered",
                                     "pending_timeout", "script_requested". */
} plh_channel_closing_args_t;

/** on_consumer_died args (HEP-CORE-0011; HEP-CORE-0023 §2.1.1).
 *  Fired on a producer when one of its registered consumers has died
 *  (process exit or heartbeat timeout); channel stays alive.  Lets
 *  producers drop per-consumer bookkeeping symmetrically. */
typedef struct
{
    const char *channel;        /**< Channel the dead consumer was on. */
    const char *consumer_uid;   /**< UID of the dead consumer presence. */
    const char *reason;         /**< "heartbeat_timeout" or "process_dead". */
} plh_consumer_died_args_t;

/** on_hub_dead args (audit D1/D2, 2026-05-18; HEP-CORE-0011;
 *  HEP-CORE-0023 §2.5; HEP-CORE-0033 §19).
 *  Fired when ZMTP declares a broker connection dead (master OR peer
 *  per HEP-0033 §19.2 multi-presence model).  When the plugin defines
 *  `on_hub_dead`, that callback REPLACES the framework's default action
 *  (master: stop the role; peer: continue on master).  The plugin may
 *  call `ctx->request_stop(ctx)` itself, mutate plugin state, attempt
 *  reconnection logic in later iterations, or do nothing. */
typedef struct
{
    const char *source_hub_uid; /**< Broker endpoint of the dead hub
                                     (the role's stable identifier for
                                     this connection per HEP-0033 §19.2). */
} plh_hub_dead_args_t;

/** on_band_member_joined args (HEP-CORE-0030 §5.3, S4 expansion 2026-05-19).
 *  Fired on every BAND_JOIN_NOTIFY received — another role joined a
 *  band this role is a member of. */
typedef struct
{
    const char *band;           /**< Band name (`!`-prefixed). */
    const char *role_uid;       /**< Joining role's UID. */
    const char *role_name;      /**< Joining role's display name (may be empty). */
} plh_band_member_joined_args_t;

/** on_band_member_left args (HEP-CORE-0030 §5.3).
 *  Fired on every BAND_LEAVE_NOTIFY received. */
typedef struct
{
    const char *band;           /**< Band name. */
    const char *role_uid;       /**< Leaving role's UID. */
    const char *reason;         /**< "voluntary", "heartbeat_timeout", or "process_dead". */
} plh_band_member_left_args_t;

/** on_band_message args (HEP-CORE-0030 §5.3).
 *  Fired on every BAND_BROADCAST_DELIVER_NOTIFY received.  Body is passed as
 *  JSON-string-serialized text (plugin must parse).  Lifetime: valid
 *  for the duration of the callback only. */
typedef struct
{
    const char *band;           /**< Band name. */
    const char *sender_role_uid;/**< Sender's role UID. */
    const char *body_json;      /**< JSON-encoded body payload. */
} plh_band_message_args_t;

/** on_band_lost args (S4 expansion 2026-05-19).
 *  Synthetic event — the framework enqueues this when the role's
 *  band routing is invalidated (currently: hub-dead drops every
 *  band_index_ entry whose Presence is on the dead connection).
 *  NOT a wire frame; reason captures why routing was lost. */
typedef struct
{
    const char *band;           /**< Band name whose routing was lost. */
    const char *reason;         /**< "hub_dead" (reserved for future
                                     additions like "evicted"). */
} plh_band_lost_args_t;

/** One authorized peer entry — used both as the `on_allowlist_changed`
 *  callback array element (HEP-CORE-0036 §I11; event-driven) AND as the
 *  visitor argument from `ctx->allowed_peers()` (polling — see
 *  native_engine_api.h §4.1; API v6 #194 Phase C).  Mirrors the wire
 *  shape of `GET_CHANNEL_AUTH_ACK.allowlist[]` and
 *  `CONSUMER_REG_ACK.producers[]` (HEP-CORE-0036 §6.4-§6.5).  Both
 *  fields are valid only for the duration of the callback / visitor
 *  call.  Plugin MUST NOT retain past return; strdup if a copy is
 *  needed.  See HEP-CORE-0028 §4.8 Lifetime + Security Contract. */
typedef struct
{
    const char *role_uid;       /**< Operator-assigned role identifier. */
    const char *pubkey_z85;     /**< Z85-encoded 40-char CURVE pubkey. */
} plh_allowed_peer_t;

/** Visitor signature for `ctx->allowed_peers()` iteration (API v6 #194
 *  Phase C).  Called once per authorized peer in the channel's
 *  allowlist.  Visitor MUST be `noexcept` — throwing across the C ABI
 *  is undefined behaviour.  Host wraps the iteration loop in a
 *  try/catch as defense-in-depth and aborts (returns -1) on a caught
 *  exception, but plugin authors must not rely on this safety net.
 *  See HEP-CORE-0028 §4.8. */
typedef void (*plh_allowed_peer_visitor)(const plh_allowed_peer_t *peer,
                                         void *userdata);

/** on_allowlist_changed args (HEP-CORE-0036 §I11; #194).  Fired on the
 *  PRODUCER side after the framework atomically applies a new
 *  authorized-consumer snapshot to the queue's ZAP cache via
 *  `ZmqQueue::set_peer_allowlist`.  Plugin may use this to coordinate
 *  startup (wait for N consumers) or react to revocation; the
 *  framework is the sole owner of admission decisions.
 *
 *  Lifetime contract: the `args` struct, the `peers` array, and all
 *  `const char *` fields are valid ONLY for the duration of this
 *  call.  Plugin MUST NOT retain pointers past return; copy
 *  (strdup or equivalent) if state must persist. */
typedef struct
{
    const char *channel;             /**< Channel whose allowlist changed. */
    const plh_allowed_peer_t *peers; /**< Authorized peers array. */
    size_t                    peer_count; /**< Number of entries in peers[]. */
    const char *reason;              /**< Wire string from the triggering
                                          NOTIFY: "consumer_joined",
                                          "consumer_left",
                                          "heartbeat_timeout", etc. */
} plh_allowlist_changed_args_t;

/** One band member — used by the `ctx->band_members()` visitor (API v6
 *  #194 Phase C).  Symmetric to plh_allowed_peer_t: same lifetime
 *  contract; same visitor-pattern shape so plugin authors learn one
 *  pattern and know both subsystems.  Mirrors the field layout of the
 *  `on_band_member_joined` callback args (see
 *  plh_band_member_joined_args_t above). */
typedef struct
{
    const char *role_uid;       /**< Member's role UID. */
    const char *role_name;      /**< Member's display name; may be NULL or
                                     empty if broker did not supply one. */
} plh_band_member_t;

/** Visitor signature for `ctx->producers()` / `ctx->consumers()`
 *  iteration (HEP-CORE-0028 §6a.5).  Called once per LIVE peer
 *  (past first-heartbeat) on the channel; role_uid string is valid
 *  for the duration of this call only (copy if state must persist).
 *  Visitor MUST be `noexcept` — same rules as
 *  `plh_allowed_peer_visitor` above.  Symmetric shape lets plugin
 *  authors learn one pattern for LIVE-peer inspection across both
 *  producer and consumer sides. */
typedef void (*plh_role_uid_visitor)(const char *role_uid,
                                     void       *userdata);

/** Visitor signature for `ctx->band_members()` iteration (API v6 #194
 *  Phase C).  Same noexcept + lifetime rules as
 *  plh_allowed_peer_visitor. */
typedef void (*plh_band_member_visitor)(const plh_band_member_t *member,
                                        void *userdata);

#endif /* PYLABHUB_NATIVE_INVOKE_TYPES_H */
