#pragma once
/**
 * @file hub_config_workers.h
 * @brief Worker forward declarations for HubConfig L2 test suite.
 */

namespace pylabhub::tests::worker::hub_config
{

/// Full hub.json: every section populated. Verify all 9 accessors return
/// the expected values, including known_roles + federation peers arrays.
int load_full(const char *tmpdir);

/// Minimal hub.json (just the required "hub" identity block); verify
/// network/admin/broker/federation/state accessors return their defaults.
int load_minimal(const char *tmpdir);

/// Unknown top-level key → std::runtime_error("hub: unknown config key '<x>'").
int strict_unknown_top_level(const char *tmpdir);

/// Unknown key inside a sub-section (e.g. "admin.bogus") → throws with
/// the qualified key path in the message.
int strict_unknown_in_section(const char *tmpdir);

/// Section that is not a JSON object (e.g. "network": 7) → throws.
int section_not_object(const char *tmpdir);

/// Missing hub.uid → auto-generated `hub.<name>.uid<8hex>` per HEP-0033 §G2.2.0a;
/// resulting uid validates under IdentifierKind::PeerUid.
int uid_auto_generated(const char *tmpdir);

/// `state.disconnected_grace_ms = -1` → resolves to kInfiniteGrace.
int state_grace_sentinel(const char *tmpdir);

/// `load_from_directory(<dir>)` resolves to `<dir>/hub.json` and returns
/// equivalent state to `load(<dir>/hub.json)`.
int load_from_directory(const char *tmpdir);

/// `reload_if_changed()` returns false when file unchanged; returns true
/// and surfaces the new values after rewriting the file.
int reload_if_changed(const char *tmpdir);

} // namespace pylabhub::tests::worker::hub_config
