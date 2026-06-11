#pragma once
/**
 * @file native_engine.hpp
 * @brief NativeEngine — ScriptEngine implementation for native C/C++ shared libraries.
 *
 * Loads a shared library (.so/.dll) via dlopen/LoadLibrary, resolves
 * callback symbols, and dispatches invoke calls as direct function pointer
 * calls with zero marshaling overhead.
 *
 * Native engine API: see src/include/utils/native_engine_api.h
 * Specification: see HEP-CORE-0028
 */

#include "pylabhub_utils_export.h"
#include "utils/script_engine.hpp"
#include "utils/native_invoke_types.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace pylabhub::hub_host { class HubAPI; }  // fwd decl

namespace pylabhub::scripting
{

class PYLABHUB_UTILS_EXPORT NativeEngine : public ScriptEngine
{
  public:
    NativeEngine();
    ~NativeEngine() override;

    NativeEngine(const NativeEngine &) = delete;
    NativeEngine &operator=(const NativeEngine &) = delete;

    // ── ScriptEngine lifecycle ───────────────────────────────────────────

    bool init_engine_(const std::string &log_tag, RoleHostCore *core) override;
    bool load_script(const std::filesystem::path &script_dir,
                     const std::string &entry_point,
                     const std::string &required_callback) override;
    bool build_api_(RoleAPIBase &api) override;
    /// Hub-side native engine support (audit B13, 2026-05-21).  Wires
    /// a minimal hub-flavoured PlhNativeContext (log + identity +
    /// request_stop; role-side function pointers are nullptr) and
    /// calls `native_init` on the plugin.  Allows `script.type:
    /// "native"` in hub.json to load + run.
    bool build_api_(hub_host::HubAPI &api) override;
    void finalize_engine_() override;

    // ── Queries ──────────────────────────────────────────────────────────

    /// THREAD-SAFETY: any thread.  Reads atomic-loadable function
    /// pointers (set once at `load_script` time, not mutated after);
    /// no language-runtime state involvement.  See HEP-CORE-0011
    /// §"Engine Thread Affinity" + Tier 1 in
    /// `docs/tech_draft/engine_callback_tiers.md`.  Overrides the
    /// base-class `ScriptEngine::has_callback` cache lookup —
    /// NativeEngine doesn't populate the cache; the function-pointer
    /// check is faster and equally thread-safe.
    [[nodiscard]] bool has_callback(const std::string &name) const noexcept override;

    // ── Schema / type building ───────────────────────────────────────────

    bool register_slot_type(const hub::SchemaSpec &spec,
                            const std::string &type_name,
                            const std::string &packing) override;
    [[nodiscard]] size_t type_sizeof(const std::string &type_name) const override;

    // ── Callback invocation ──────────────────────────────────────────────

    void invoke_on_init() override;
    void invoke_on_stop() override;
    void invoke_on_channel_closing(const std::string &channel,
                                    const std::string &reason) override;
    void invoke_on_consumer_died(const std::string &channel,
                                  const std::string &consumer_uid,
                                  const std::string &reason) override;
    void invoke_on_hub_dead(const std::string &source_hub_uid) override;
    void invoke_on_band_member_joined(const std::string &band,
                                      const std::string &role_uid,
                                      const std::string &role_name) override;
    void invoke_on_band_member_left(const std::string &band,
                                    const std::string &role_uid,
                                    const std::string &reason) override;
    void invoke_on_band_message(const std::string &band,
                                const std::string &sender_role_uid,
                                const nlohmann::json &body) override;
    void invoke_on_band_lost(const std::string &band,
                             const std::string &reason) override;
    /// HEP-CORE-0036 §I11 — Native plugins currently do not implement
    /// the allowlist-changed callback (Native is MVP per #84/#85);
    /// scripts using polling via `RoleAPIBase::allowed_peers` still
    /// work.  Override is a stub that satisfies the pure virtual.
    void invoke_on_allowlist_changed(
        const std::string &channel,
        const std::vector<AllowedPeer> &allowlist,
        const std::string &reason) override;

    InvokeResult invoke_produce(
        InvokeTx tx,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_consume(
        InvokeRx rx,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_process(
        InvokeRx rx, InvokeTx tx,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_on_inbox(
        InvokeInbox msg) override;

    // ── Generic invoke ───────────────────────────────────────────────────

    bool invoke(const std::string &name) override;
    bool invoke(const std::string &name, const nlohmann::json &args) override;
    InvokeResponse eval(const std::string &code) override;
    /// HEP-CORE-0033 §12.2.2 augmentation hook entry point.  Native
    /// engines have no script-side return-value capture surface today
    /// (compiled C plugins use the typed cycle ops instead) — this
    /// returns NotFound so HubAPI::augment_* leaves the default
    /// response unchanged.  `process_pending` inherits the base
    /// no-op default — there's no cross-thread pending queue here.
    InvokeResponse invoke_returning(const std::string &name,
                                    const nlohmann::json &args,
                                    int64_t timeout_ms = -1) override;

    // ── Error state ──────────────────────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const noexcept override;

    // ── Threading ────────────────────────────────────────────────────────

    [[nodiscard]] bool supports_multi_state() const noexcept override;
    void release_thread() override {}

    /// Set the expected BLAKE2b-256 hex checksum for the native .so file.
    /// If set, load_script() verifies the file hash before dlopen.
    void set_expected_checksum(std::string hex_checksum);

  private:
    std::string log_tag_;

    // ── Dynamic library handle ──────────────────────────────────────────
    void *dl_handle_{nullptr};

    // ── Resolved function pointers (C linkage) ──────────────────────────
    // Native callback ABI (uniform shapes):
    //   - No-args lifecycle (on_init / on_stop / on_heartbeat):
    //         `void(*)(void)`   -> FnVoidNoArgs
    //   - Structured-args lifecycle (on_channel_closing / on_consumer_died):
    //         `void(*)(const plh_X_args_t *)`   -> typed per event
    //   - Hot-path data callbacks (on_produce / on_consume / on_process / on_inbox):
    //         `bool(*)(const plh_X_t *)`   -> typed per direction, commit/discard
    //   - Ad-hoc admin/generic invoke (any plugin-defined symbol):
    //         `void(*)(const char *args_json)`   -> FnVoid (JSON-string ABI)
    //     Lifecycle callbacks DO NOT use the FnVoid shape; the JSON-
    //     string ABI is reserved for the generic `invoke(name, args)`
    //     surface where the callee is plugin-defined and not on the
    //     standard lifecycle list.
    using FnNativeInit       = bool (*)(const void *ctx);
    using FnNativeFinalize   = void (*)();
    using FnVoidNoArgs       = void (*)();
    using FnVoid             = void (*)(const char *args_json);
    using FnOnChannelClosing   = void (*)(const plh_channel_closing_args_t *);
    using FnOnConsumerDied     = void (*)(const plh_consumer_died_args_t *);
    using FnOnHubDead          = void (*)(const plh_hub_dead_args_t *);
    using FnOnBandMemberJoined = void (*)(const plh_band_member_joined_args_t *);
    using FnOnBandMemberLeft   = void (*)(const plh_band_member_left_args_t *);
    using FnOnBandMessage      = void (*)(const plh_band_message_args_t *);
    using FnOnBandLost         = void (*)(const plh_band_lost_args_t *);
    using FnOnAllowlistChanged = void (*)(const plh_allowlist_changed_args_t *);
    using FnOnProduce        = bool (*)(const plh_tx_t *);
    using FnOnConsume        = bool (*)(const plh_rx_t *);
    using FnOnProcess        = bool (*)(const plh_rx_t *, const plh_tx_t *);
    using FnOnInbox          = bool (*)(const plh_inbox_msg_t *);
    using FnSchemaDesc       = const char *(*)();
    using FnSizeof           = size_t (*)();
    using FnBool             = bool (*)();
    using FnCstr             = const char *(*)();

    FnNativeInit       fn_init_{nullptr};
    FnNativeFinalize   fn_finalize_{nullptr};
    FnVoidNoArgs       fn_on_init_{nullptr};
    FnVoidNoArgs       fn_on_stop_{nullptr};
    FnOnChannelClosing   fn_on_channel_closing_{nullptr};
    FnOnConsumerDied     fn_on_consumer_died_{nullptr};
    FnOnHubDead          fn_on_hub_dead_{nullptr};
    // S4 expansion 2026-05-19 — typed band callbacks.
    FnOnBandMemberJoined fn_on_band_member_joined_{nullptr};
    FnOnBandMemberLeft   fn_on_band_member_left_{nullptr};
    FnOnBandMessage      fn_on_band_message_{nullptr};
    FnOnBandLost         fn_on_band_lost_{nullptr};
    // HEP-CORE-0036 §I11 (#194, 2026-06-10).  v3 → v4 API bump.
    FnOnAllowlistChanged fn_on_allowlist_changed_{nullptr};
    FnOnProduce        fn_on_produce_{nullptr};
    FnOnConsume        fn_on_consume_{nullptr};
    FnOnProcess        fn_on_process_{nullptr};
    FnOnInbox          fn_on_inbox_{nullptr};
    FnVoidNoArgs       fn_on_heartbeat_{nullptr};
    FnBool             fn_is_thread_safe_{nullptr};

    // ── Schema tracking ─────────────────────────────────────────────────
    std::unordered_map<std::string, size_t> type_sizes_;

    // ── Native context (kept alive for engine lifetime) ─────────────────
    struct NativeContextStorage;
    std::unique_ptr<NativeContextStorage> native_ctx_;

    // ── File integrity ──────────────────────────────────────────────────────
    std::string expected_checksum_;  ///< BLAKE2b-256 hex, empty = skip check

    // ── Lifecycle ───────────────────────────────────────────────────────────
    std::string lifecycle_module_name_;  ///< Unique name for lifecycle registration
    bool lifecycle_registered_{false};   ///< True if module was registered
    std::filesystem::path lib_path_;

    // ── Internal helpers ────────────────────────────────────────────────
    void *resolve_sym_(const char *name) const;
    bool verify_abi_() const;
    bool verify_file_checksum_() const;
    std::string compute_canonical_schema_(const hub::SchemaSpec &spec) const;
};

} // namespace pylabhub::scripting
