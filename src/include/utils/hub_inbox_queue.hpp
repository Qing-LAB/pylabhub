#pragma once
/**
 * @file hub_inbox_queue.hpp
 * @brief InboxQueue (ROUTER receiver) and InboxClient (DEALER sender) for typed role-to-role
 * messaging.
 *
 * ## Overview
 * A role declares an inbox by configuring `inbox_schema` in its JSON config.
 * The inbox endpoint is registered with the broker at startup (REG_REQ `inbox_endpoint` field).
 * Any role can discover the inbox endpoint via ROLE_INFO_REQ (Phase 4) and connect an InboxClient.
 *
 * ## Wire format (MessagePack fixarray[5] — the same 5-tuple as ZmqQueue,
 *    packed by the shared `wire_detail::pack_frame` codec)
 *   [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N fields), checksum:bin32]
 *   - magic      : 0x51484C50 ('PLHQ')
 *   - schema_tag : first 8 bytes of BLAKE2b-256 over the inbox canonical
 *                  form (HEP-CORE-0034 §11.4 — `type:count:length;...|pack:`,
 *                  computed by `compute_inbox_schema_tag`).  Note: this is
 *                  a different canonical form than slot/flexzone (§6.3)
 *                  because inbox messages are envelope-typed and carry no
 *                  field names on the wire.  Optional identity guard.
 *   - seq        : monotonic sender counter
 *   - payload    : N typed field values (scalar or bin)
 *   - checksum   : bin32 — BLAKE2b-256 over the decrypted message content,
 *                  per the owner-chosen `ChecksumPolicy` (None/Manual/Enforced);
 *                  defence-in-depth on top of the mandatory CURVE transport
 *                  (HEP-CORE-0027 §3).  `None` sends zeros / skips verify.
 *
 * ## ZMQ framing (ROUTER-DEALER)
 * InboxClient sets ZMQ_IDENTITY to the sender's pylabhub UID before connecting.
 * ROUTER receives: [identity_frame, empty_frame, payload_frame]
 * ROUTER sends ACK: [identity_frame, empty_frame, ack_code_byte]
 * DEALER receives ACK: ["", ack_code_byte]  (ZMQ strips identity; app drains empty delimiter)
 *
 * ## Thread safety
 * InboxQueue is designed for use from a SINGLE inbox_thread_. All recv_one() and
 * send_ack() calls must come from the same thread (the ROUTER socket is not thread-safe).
 * InboxClient is also single-threaded: acquire()/send()/abort() from one caller thread.
 *
 * ## ZMQ context
 * Sockets are created from the shared process-wide zmq::context_t owned by the
 * `ZMQContext` lifecycle module (utils/zmq_context.hpp). Neither InboxQueue nor
 * InboxClient creates or terminates the context — the top-level LifecycleGuard
 * must include `pylabhub::hub::GetZMQContextModule()`.
 *
 * ## Lifecycle
 * Call start() before recv_one()/acquire(); call stop() before destruction.
 * stop() closes the socket (RAII via cppzmq). The shared ZMQ context is NOT
 * closed here — it outlives every InboxQueue/InboxClient in the process.
 */
#include "utils/hub_zmq_queue.hpp"           // ZmqSchemaField
#include "utils/security/attested_key.hpp"   // AttestedKey (what the handshake proved)
#include "utils/security/peer_admission.hpp" // PeerAdmission (inbox ROUTER ZAP)
#include "utils/security/pubkey_origin.hpp"  // AttributedSender (who that key is)

#include "pylabhub_utils_export.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct InboxQueueImpl;
struct InboxClientImpl;

// ============================================================================
// InboxItem — result of InboxQueue::recv_one()
// ============================================================================

/**
 * @struct InboxItem
 * @brief One decoded inbox message.  Valid only until the next recv_one() call.
 */
struct PYLABHUB_UTILS_EXPORT InboxItem
{
    const void *data{nullptr}; ///< Decoded payload buffer (InboxQueue-owned; item_size() bytes).

    /// Pylabhub UID of the sender, resolved from the key its CURVE handshake
    /// PROVED (HEP-CORE-0027 §3.7).
    ///
    /// Not the ZMQ routing id, which the sender chooses for itself and which
    /// is an ACK return address and nothing else.  Keying anything on that
    /// would let one sender be attributed as another, and let a sender earn a
    /// fresh replay window by changing the id it presents.
    std::string sender_id;
    uint64_t seq{0}; ///< Monotonic sender sequence number.

    /// Messages lost from THIS sender immediately before this one.
    ///
    /// A full inbox drops — that is the framework's behaviour and not a
    /// configurable policy, because queueing more for a receiver that is not
    /// draining helps nobody (HEP-CORE-0027 §3.7).  What IS the receiver's
    /// business is knowing it happened, so the loss is reported here, per
    /// message, and the handler decides: ignore it, re-request state, log it,
    /// stop.  The framework does not retry, back off, or repair on the
    /// receiver's behalf.
    ///
    /// 0 means "nothing missing since the previous message from this sender",
    /// which is the normal case.  Derived from the sequence numbers, so it
    /// counts everything lost in transit — a refused send at the far end, a
    /// frame dropped by the replay guard, any of it.
    uint64_t gap{0};
};

// ============================================================================
// InboxQueue — ZMQ ROUTER receiver
// ============================================================================

/**
 * @class InboxQueue
 * @brief ZMQ ROUTER socket that receives typed, schema-validated messages and sends ACK.
 *
 * Designed for use from a single inbox_thread_. Not thread-safe for concurrent callers.
 *
 * Typical usage in inbox_thread_:
 * @code
 *   while (running) {
 *       const auto* item = inbox_queue_->recv_one(std::chrono::milliseconds{100});
 *       if (!item) continue;
 *       uint8_t ack = 0;
 *       { py::gil_scoped_acquire g;
 *         try { call_on_inbox_(item); }
 *         catch (...) { ack = 3; } }
 *       inbox_queue_->send_ack(ack);
 *   }
 * @endcode
 */
class PYLABHUB_UTILS_EXPORT InboxQueue : public pylabhub::utils::security::PeerAdmission
{
  public:
    /**
     * @brief Factory: create an InboxQueue that RECORDS @p endpoint as the bind target.
     *        `start()` performs the bind and arms CURVE with NO admission authority
     *        bound — called by production role-host setup at S1 (HEP-CORE-0027 §4.1:
     *        binding early resolves port-0 endpoints before REG_REQ advertises them;
     *        an unbound gate admits no peer until the role wires itself in via
     *        `set_admission_authority`, and answers no until that role's first
     *        roster arrives).
     *
     * @param endpoint    ZMQ endpoint to bind (e.g. "tcp://0.0.0.0:5592" or "tcp://0.0.0.0:0").
     *                    Port 0 causes the OS to assign a free port; retrieve it via
     * bound_address().
     * @param schema      Field list — must be non-empty; returns nullptr on error.
     * @param packing     "aligned" or "packed". Must match InboxClient packing.
     * @param rcvhwm      ZMQ_RCVHWM: max messages queued for this socket per
     *                    connected peer.  Reaching it applies BACK-PRESSURE —
     *                    ZMQ does not drop here; the sender's next send fails
     *                    instead (`InboxClient::send` returns 255).  Bounds
     *                    memory at roughly rcvhwm x peers x item_size, so
     *                    raising it on a wide fan-in is a real memory
     *                    decision.  Default 1000 (ZMQ built-in default).
     *
     *                    0 means NO LIMIT to ZMQ (`pipe_t::check_hwm` tests
     *                    `_hwm > 0 &&`), i.e. unbounded growth until memory
     *                    runs out.  Role config rejects it, and the retired
     *                    `inbox_overflow_policy: "block"` used to select it
     *                    silently — which is how a setting named for
     *                    back-pressure came to mean "no back-pressure at
     *                    all".  Pass 0 only in a test that is deliberately
     *                    measuring unbounded behaviour.
     * @return            nullptr on schema or ZMQ setup error (logged internally).
     */
    [[nodiscard]] static std::unique_ptr<InboxQueue> bind_at(const std::string &endpoint,
                                                             std::vector<ZmqSchemaField> schema,
                                                             std::string packing = "aligned",
                                                             int rcvhwm = 1000);

    ~InboxQueue();
    InboxQueue(InboxQueue &&) noexcept;
    InboxQueue &operator=(InboxQueue &&) noexcept;
    InboxQueue(const InboxQueue &) = delete;
    InboxQueue &operator=(const InboxQueue &) = delete;

    /**
     * @brief Bind ROUTER socket and start listening.
     * @return true on success or if already running (idempotent).
     *         false only on actual startup failure.
     */
    bool start();

    /**
     * @brief Stop listening and close socket + context.  Safe to call multiple times.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    /**
     * @brief Where senders can actually reach this inbox, once it binds.
     *
     * For port-0 binds, the OS-assigned endpoint (e.g.
     * `tcp://127.0.0.1:54321`).  `std::nullopt` before the bind
     * completes.
     *
     * The inbox is the clearest case for the distinction this return
     * type enforces: its configured default IS `tcp://127.0.0.1:0`, so
     * "unresolved" is its normal starting condition, and an accessor
     * that fell back to the configured value would hand out port 0 as
     * though it were an address.  See HEP-CORE-0036 §6.7.2.
     */
    [[nodiscard]] std::optional<::pylabhub::BoundAddress> bound_address() const;

    /** Size in bytes of one decoded item buffer. */
    [[nodiscard]] size_t item_size() const noexcept;

    /**
     * @brief Blocking receive of one inbox message with timeout.
     *
     * @param timeout  Maximum wait time. Returns nullptr on timeout or stop().
     * @return Pointer to the decoded InboxItem (owned by this InboxQueue; valid until
     *         the next recv_one() call).  nullptr on timeout or error.
     */
    [[nodiscard]] const InboxItem *recv_one(std::chrono::milliseconds timeout) noexcept;

    /**
     * @brief Send ACK for the last recv_one() result.
     *
     * Must be called exactly once after each successful recv_one() != nullptr.
     * Uses the sender_id stored internally from the last recv_one().
     *
     * @param code  0=OK, 1=queue_overflow, 2=schema_error, 3=handler_error.
     *
     * These codes are the APPLICATION's vocabulary — the handler decides which
     * one fits and passes it here; the transport never picks one.  In
     * particular `1` refers to an application-level backlog: ZMQ's own
     * receive queue is invisible from this side (libzmq exposes no queue
     * depth), so transport back-pressure can never surface as this code.  It
     * reaches the sender as a failed send instead — see
     * `InboxClient::send_blocked_count()`.  Nothing in-tree emits `1` today.
     *
     * The ACK goes out as a full frame carrying the acknowledged message's
     * sequence number, not as a bare byte, so the sender can tell this receipt
     * from one left over by an earlier send that stopped waiting
     * (HEP-CORE-0027 §3).  The seq is taken from the last `recv_one()`, which
     * is why this must be called once per successful receive.
     */
    void send_ack(uint8_t code) noexcept;

    /** Frames rejected (bad magic, schema mismatch, field type/size error). */
    [[nodiscard]] uint64_t recv_frame_error_count() const noexcept;
    /** ACK send failures (ZMQ errors). */
    [[nodiscard]] uint64_t ack_send_error_count() const noexcept;
    /** Sequence number gaps detected (sender restarts or dropped frames). */
    [[nodiscard]] uint64_t recv_gap_count() const noexcept;
    /** BLAKE2b checksum verification failures. */
    [[nodiscard]] uint64_t checksum_error_count() const noexcept;
    /** Frames dropped by the replay guard (skew or nonce reuse; §3.6). */
    [[nodiscard]] uint64_t recv_replay_reject_count() const noexcept;

    /** Set checksum policy. Enforced = auto verify on recv. None = skip. */
    void set_checksum_policy(ChecksumPolicy policy) noexcept;

    /// Snapshot inbox metrics.
    struct InboxMetricsSnapshot
    {
        uint64_t recv_frame_error_count{0};
        uint64_t ack_send_error_count{0};
        uint64_t recv_gap_count{0};
        uint64_t checksum_error_count{0};
        uint64_t recv_replay_reject_count{0};
    };
    [[nodiscard]] InboxMetricsSnapshot inbox_metrics() const noexcept
    {
        return {recv_frame_error_count(), ack_send_error_count(), recv_gap_count(),
                checksum_error_count(), recv_replay_reject_count()};
    }

    /**
     * @brief Arm CURVE-server authentication on the inbox ROUTER
     *        (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3).  MUST be called
     *        before start().
     *
     * The inbox is a hub-wide role↔role facility, so the ROUTER binds
     * as a CURVE server under its OWN @p zap_domain (distinct from the
     * data channel's), authorizing against the hub-wide roster rather
     * than any channel allowlist.  That roster is the operator's vault
     * entries intersected with the roles the hub currently has
     * registered (HEP-CORE-0035 §4.9.2), and it lives on the role, not
     * here — see set_admission_authority().  Until one is bound,
     * start() binds deny-all (secure default — no peer completes the
     * handshake).
     *
     * @param identity_key_name  KeyStore key name for the role's
     *        identity keypair (security::kRoleIdentityName — the same
     *        keypair the data sockets present, single-key model I6).
     * @param zap_domain         Distinct inbox ZAP domain, e.g.
     *        "<uid>:inbox".  Registered with the process ZapRouter.
     */
    void set_curve_server_identity(std::string identity_key_name, std::string zap_domain);

    // ── PeerAdmission (HEP-CORE-0036 §7) — inbox ROUTER ZAP gate ──────────
    // The gate asks; it does not hold.  The hub-wide roster lives on the
    // role (HEP-CORE-0035 §4.9.6), and the ZapRouter pump thread reaches it
    // through the bound authority below at handshake time.

    /// Does any authority the role holds vouch for this key?
    ///
    /// Takes the key alone, not a `PeerIdentity`: the mechanism is this
    /// transport's business, and the role answers about keys.
    using AdmitsPubkey = std::function<bool(const std::string &pubkey_z85)>;

    /// Who is the connection this message arrived on?
    ///
    /// Takes the attestation rather than a key string, because the answer
    /// names a principal and a name may only be derived from a key the
    /// transport vouched for.  `AttestedKey` is mintable in exactly one
    /// place (`AttestedKey::from_message`); accepting a bare string here
    /// would let any caller supply one and be believed.
    using AttributesSender = std::function<pylabhub::utils::security::AttributedSender(
        const std::optional<pylabhub::utils::security::AttestedKey> &attested)>;

    /// The two questions this gate asks about a peer — may it connect, and
    /// who is it (HEP-CORE-0035 §4.9.6).
    ///
    /// Bound together and never separately.  They are answered from the same
    /// snapshots, so binding them in two calls would create two things that
    /// must both be done and could disagree — and a half-bound gate would
    /// admit peers it cannot name, which is the defect this type exists to
    /// make unrepresentable (I-ROSTER-ASK-DONT-COPY).
    ///
    /// They take different arguments because their callers hold different
    /// things: at handshake time no attestation exists yet — deciding
    /// whether to attest IS the question — so the ZAP side must answer about
    /// a key, while the message side answers about a proven one.
    struct InboxAuthority
    {
        InboxAuthority(AdmitsPubkey a, AttributesSender n)
            : admits(std::move(a)), name_of(std::move(n))
        {
        }

        AdmitsPubkey admits;
        AttributesSender name_of;
    };

    /// Bind the questions above.  Callable before or after `start()`, and
    /// concurrently with `is_peer_allowed`.
    ///
    /// A role binds this instead of pushing its roster down, because a copy
    /// parked here would be a second representation of the hub's key list,
    /// free to disagree with the role's own — and the stale one would be the
    /// one the ZAP handler consults.  Until an authority is bound the gate
    /// denies everyone and names nobody, which is the same rule the hub
    /// applies to itself (HEP-CORE-0035 §4.8.4): no configuration admits no
    /// one.
    void set_admission_authority(InboxAuthority authority);

    /// Inert — returns false.  This gate keeps no list to replace; see
    /// `set_admission_authority`.  Refused rather than accepted-and-ignored
    /// so a caller expecting the push to gate something finds out.
    bool set_peer_allowlist(pylabhub::utils::security::PeerAllowlist allowlist) override;

    /// Always `std::nullopt` — there is no stored list to hand back, and
    /// the authority answers about one key at a time by design (it cannot
    /// be enumerated).
    [[nodiscard]] std::optional<pylabhub::utils::security::PeerAllowlist>
    peer_allowlist_snapshot() const override;

    /// CURVE peers only; delegates to the bound authority.  Runs on the ZAP
    /// pump thread — synchronously, as the reentrance contract requires.
    [[nodiscard]] bool
    is_peer_allowed(const pylabhub::utils::security::PeerIdentity &peer) const override;

  private:
    explicit InboxQueue(std::unique_ptr<InboxQueueImpl> impl);
    std::unique_ptr<InboxQueueImpl> pImpl;
};

// ============================================================================
// InboxClient — ZMQ DEALER sender
// ============================================================================

/**
 * @class InboxClient
 * @brief ZMQ DEALER socket that sends typed, schema-validated messages to an InboxQueue.
 *
 * The ZMQ_IDENTITY is set to @p sender_uid before connecting, so the ROUTER
 * sees the sender's pylabhub UID as the routing identity.
 *
 * Typical usage:
 * @code
 *   auto client = InboxClient::connect_to(endpoint, my_uid, schema);
 *   client->start();
 *   void* buf = client->acquire();
 *   // fill buf with typed fields
 *   uint8_t ack = client->send(std::chrono::milliseconds{500});
 * @endcode
 */
class PYLABHUB_UTILS_EXPORT InboxClient
{
  public:
    /**
     * @brief Factory: create an InboxClient that connects a DEALER to @p endpoint.
     *
     * @param endpoint    ZMQ endpoint of the ROUTER InboxQueue.
     * @param sender_uid  This client's pylabhub UID — set as ZMQ_IDENTITY before connect.
     * @param schema      Field list — must be non-empty; returns nullptr on error.
     * @param packing     "aligned" or "packed". Must match InboxQueue packing.
     * @return            nullptr on schema or ZMQ setup error (logged internally).
     */
    [[nodiscard]] static std::unique_ptr<InboxClient> connect_to(const std::string &endpoint,
                                                                 const std::string &sender_uid,
                                                                 std::vector<ZmqSchemaField> schema,
                                                                 std::string packing = "aligned");

    ~InboxClient();
    InboxClient(InboxClient &&) noexcept;
    InboxClient &operator=(InboxClient &&) noexcept;
    InboxClient(const InboxClient &) = delete;
    InboxClient &operator=(const InboxClient &) = delete;

    /**
     * @brief Set ZMQ_IDENTITY and connect DEALER socket to the configured endpoint.
     * @return true on success or if already running (idempotent).
     *         false only on actual startup failure.
     */
    bool start();

    /**
     * @brief Disconnect and close socket + context.  Safe to call multiple times.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    /** Size in bytes of one send buffer. */
    [[nodiscard]] size_t item_size() const noexcept;

    /**
     * @brief Get the write buffer.  Fill it with typed field values before calling send().
     * The buffer is zero-initialized at factory time and between sends.
     * @return Pointer to the write buffer (item_size() bytes); nullptr if not running.
     */
    [[nodiscard]] void *acquire() noexcept;

    /**
     * @brief Encode and send the buffer contents; optionally wait for ACK.
     *
     * The transmit itself never blocks.  If the socket has no writable peer —
     * the receiver's queue is at its high-water mark, or the peer is not
     * connected / was denied at the CURVE handshake — the message is dropped
     * and 255 is returned immediately rather than parking the caller.  This
     * is what makes `ack_timeout` the whole bound on this call.
     *
     * Dropping is deliberate: a receiver that is not draining its inbox is not
     * helped by queueing more, and the alternative (wait for room) hands the
     * caller's liveness to the slowest peer.  What to do about it is the
     * caller's decision, not this class's.
     *
     * @param ack_timeout  If > 0 ms: block up to this duration for the ACK byte.
     *                     If 0 ms: fire-and-forget (returns 0 immediately).
     * @return ACK error code (0=OK, non-zero=error).  Returns 255 on send failure or ACK timeout.
     */
    uint8_t send(std::chrono::milliseconds ack_timeout = std::chrono::milliseconds{1000}) noexcept;

    /**
     * @brief Sends dropped because the socket had no writable peer.
     *
     * Counts every occurrence, whereas the log is edge-triggered (one line
     * when the path blocks, one when it recovers) so a persistently blocked
     * peer cannot flood the log.  A rising count with no new log line means
     * the condition never cleared.
     */
    [[nodiscard]] uint64_t send_blocked_count() const noexcept;

    /**
     * @brief ACKs discarded because they belonged to an earlier send.
     *
     * Each ACK carries the sequence number of the message it acknowledges, so
     * a receipt arriving after its own send already timed out is recognised
     * and dropped instead of being reported as the current message's result.
     * A rising count means `ack_timeout` is tighter than the receiver's real
     * turnaround — the sends are succeeding, the caller is just not waiting
     * long enough to hear about it.
     */
    [[nodiscard]] uint64_t ack_stale_count() const noexcept;

    /**
     * @brief Discard the current buffer contents without sending.  Next acquire() is fresh.
     */
    void abort() noexcept;

    /** Set checksum policy. Enforced = auto compute on send. None = send zeros. */
    void set_checksum_policy(ChecksumPolicy policy) noexcept;

    /**
     * @brief Arm CURVE-client authentication on the inbox DEALER
     *        (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3).  MUST be called
     *        before start().
     *
     * The DEALER presents the sender role's identity keypair and pins
     * the receiver's identity pubkey as `curve_serverkey`.  The receiver
     * pubkey is discovered via ROLE_INFO_ACK, which the hub answers only
     * once the receiver holds a roster naming this sender (HEP-CORE-0035
     * §4.9.7) — so by the time this is armed, the receiver's ROUTER is
     * expected to admit it.  A refusal is terminal: libzmq tears the
     * connection down and nothing retries it, which is why reachability
     * is settled before the dial rather than discovered by attempting.
     *
     * @param identity_key_name  KeyStore key name for the sender's
     *        identity keypair (security::kRoleIdentityName).
     * @param server_pubkey_z85  The receiver's identity pubkey (Z85),
     *        set as curve_serverkey.
     */
    void set_curve_client_identity(std::string identity_key_name, std::string server_pubkey_z85);

  private:
    explicit InboxClient(std::unique_ptr<InboxClientImpl> impl);
    std::unique_ptr<InboxClientImpl> pImpl;
};

} // namespace pylabhub::hub

/**
 * @brief Canonical field list for InboxQueue::InboxMetricsSnapshot serialization.
 * Same X-macro pattern as PYLABHUB_QUEUE_METRICS_FIELDS (see hub_queue.hpp).
 */
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define PYLABHUB_INBOX_METRICS_FIELDS(X)                                                           \
    X(recv_frame_error_count)                                                                      \
    X(ack_send_error_count)                                                                        \
    X(recv_gap_count)                                                                              \
    X(checksum_error_count)                                                                        \
    X(recv_replay_reject_count)
// NOLINTEND(cppcoreguidelines-macro-usage)
