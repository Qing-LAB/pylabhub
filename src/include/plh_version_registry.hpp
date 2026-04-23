#pragma once
/**
 * @file plh_version_registry.hpp
 * @brief Centralized version registry and ABI-compatibility check.
 *
 * Tracks seven independent ABI/interface surfaces.  Each one covers a
 * distinct boundary where silent drift between two parties can corrupt
 * behaviour; each one has its own bump cadence.
 *
 * Exposed to scripts via `version_info_json()`; available to binaries
 * via `check_abi(abi_expected_here())` for startup compatibility
 * assertion.  See `docs/tech_draft/abi_check_facility_design.md` and
 * HEP-CORE-0026 (registry) / HEP-CORE-0032 (ABI compatibility).
 *
 * ## Versioning axes
 *
 * | Axis            | Boundary                            | Carrier format |
 * |-----------------|-------------------------------------|----------------|
 * | `library`       | Binary ↔ shared library (SO)        | SOVERSION |
 * | `shm`           | Producer ↔ Consumer (same SHM)      | `SharedMemoryHeader` binary layout |
 * | `broker_proto`  | Role ↔ Broker (control plane)       | JSON over ZMQ multipart `['C'][msg_type][json]` |
 * | `zmq_frame`     | Role ↔ Role (ZMQ data plane)        | msgpack `[magic, tag, seq, payload, checksum]` |
 * | `script_api`    | Framework ↔ Python/Lua scripts      | pybind11 + LuaJIT API |
 * | `script_engine` | Framework ↔ NativeEngine plugins    | C++ `ScriptEngine` virtual interface |
 * | `config`        | User JSON files ↔ Binary            | allowed-keys sets per parser |
 *
 * ## Bump rules (same for every axis)
 *
 * - Major: breaking change — removed/renamed field or method, changed
 *   semantics, changed layout or encoding.  Caller with mismatched
 *   major must reject at the boundary.
 * - Minor: additive change — new optional field, new method with base
 *   default.  Caller with lower minor logs WARN and proceeds.
 *
 * ## Usage
 *
 * Read the current library's view:
 * @code{.cpp}
 *   LOGGER_INFO("Starting: {}", pylabhub::version::version_info_string());
 * @endcode
 *
 * Assert startup compatibility (pattern for every binary's main()):
 * @code{.cpp}
 *   constexpr auto kExpected = pylabhub::version::abi_expected_here();
 *   const auto r = pylabhub::version::check_abi(
 *       kExpected.versions, kExpected.build_id);
 *   if (!r.compatible) { std::fprintf(stderr, "%s\n", r.message.c_str());
 *                        return 2; }
 * @endcode
 */

#include "pylabhub_utils_export.h"
#include "pylabhub_version.h"

#ifdef PYLABHUB_HAVE_BUILD_ID
#include "pylabhub_build_id.h"
#endif

#include <cstdint>
#include <string>

namespace pylabhub::version
{

/**
 * @brief Compile-time version constants for every independently-versioned
 *        ABI surface.
 *
 * Each major+minor pair follows the semantic rules documented at the top
 * of this header.  See `kXxxMajor/Minor` below for per-axis bump history.
 */
struct ComponentVersions
{
    // --- Library identity (CMake project() VERSION + git rev-list --count) ---
    uint16_t library_major;
    uint16_t library_minor;
    uint16_t library_rolling;

    // --- Shared-memory header layout (mirrors data_block.hpp
    //     HEADER_VERSION_*).  Bump major on SharedMemoryHeader layout
    //     change or offset shuffle; minor on added trailing reserved
    //     bytes that old readers can safely ignore. ---
    uint8_t shm_major;
    uint8_t shm_minor;

    // --- Broker control-plane protocol (was "wire" before 2026-04-22).
    //     Covers the JSON message set between BrokerRequestComm and
    //     BrokerService: REG_REQ, DISC_REQ, DISC_ACK, HEARTBEAT_REQ,
    //     CHANNEL_CLOSING_NOTIFY, FORCE_SHUTDOWN, ROLE_PRESENCE_REQ,
    //     ROLE_INFO_REQ, METRICS_REPORT_REQ, CHECKSUM_ERROR_REPORT,
    //     HUB_PEER_HELLO/BYE, HUB_RELAY_MSG, CONSUMER_REG_REQ, ...
    //     Spec: HEP-CORE-0007.  Bump major on removed/renamed fields or
    //     changed semantics; minor on added optional fields. ---
    uint8_t broker_proto_major;
    uint8_t broker_proto_minor;

    // --- ZMQ data-plane envelope (ZmqQueue + InboxQueue shared via
    //     src/utils/hub/zmq_wire_helpers.hpp).  Covers the msgpack
    //     `[magic, schema_tag, seq, payload_array, checksum]` envelope.
    //     Bump major on envelope restructure (array length change,
    //     checksum algorithm, magic change); minor on added optional
    //     trailing elements. ---
    uint8_t zmq_frame_major;
    uint8_t zmq_frame_minor;

    // --- Python/Lua script API surface.  Bump major on removed/renamed
    //     methods or behavioural changes; minor on new methods or
    //     properties. ---
    uint8_t script_api_major;
    uint8_t script_api_minor;

    // --- ScriptEngine C++ virtual-interface version (LuaEngine,
    //     PythonEngine, NativeEngine all derive from ScriptEngine).
    //     Bump major on removed/renamed virtual, signature change, or
    //     pure-virtual-with-no-default added; minor on additive virtual
    //     with base-class default.  1.0 → 1.1 on the 2026-04-21
    //     addition of `pending_script_engine_request_count()`. ---
    uint8_t script_engine_major;
    uint8_t script_engine_minor;

    // --- JSON config file schema.  Covers the allowed-keys sets
    //     enforced by role_config.cpp + the nested whitelists in
    //     script_config / auth_config / identity_config /
    //     startup_config / logging_config.  Bump major on removed or
    //     renamed key; minor on added optional key. ---
    uint8_t config_major;
    uint8_t config_minor;
};

// ============================================================================
// Per-axis constants (inline constexpr so consteval callers can fold)
//
// The `shm` values mirror `pylabhub::hub::detail::HEADER_VERSION_MAJOR/MINOR`
// in data_block.hpp.  A static_assert in version_registry.cpp pins the
// equality so a drift in either location is a compile error.
// ============================================================================

inline constexpr uint8_t kShmMajor             = 1;
inline constexpr uint8_t kShmMinor             = 0;
inline constexpr uint8_t kBrokerProtoMajor     = 1;
inline constexpr uint8_t kBrokerProtoMinor     = 0;
inline constexpr uint8_t kZmqFrameMajor        = 1;
inline constexpr uint8_t kZmqFrameMinor        = 0;
inline constexpr uint8_t kScriptApiMajor       = 1;
inline constexpr uint8_t kScriptApiMinor       = 0;
// script_engine 1.0 → 1.1: pending_script_engine_request_count() added
// with base-class default returning 0 (2026-04-21, commit 4e30fa3).
// script_engine 1.1 → 1.2: RoleHostBase (non-template class) promoted
// to EngineHost<ApiT> (template), with `using RoleHostBase =
// EngineHost<RoleAPIBase>;` preserving the source-level name
// (2026-04-23, HEP-0033 G1 prereq).  Source-compatible for derived
// classes and any code using `RoleHostBase` by name; symbol-level
// mangled names changed (pre-refactor binaries linking against the new
// library get unresolved-symbol failures).  The build_id strict check
// under PYLABHUB_STRICT_ABI_CHECK / Debug catches stale-binary cases;
// this axis bump is the declared-axis signal for the rename.
inline constexpr uint8_t kScriptEngineMajor    = 1;
inline constexpr uint8_t kScriptEngineMinor    = 2;
// config 1.0 → 1.1: nested-key whitelist rolled out across 4 sub-parsers
// (script/auth/identity/startup, 2026-04-22, commit fffd095).  Additive
// rejection behaviour — any existing valid config still loads; only
// previously-silent typos now raise at load time.
inline constexpr uint8_t kConfigMajor          = 1;
inline constexpr uint8_t kConfigMinor          = 1;

// ============================================================================
// Compile-time capture of the caller's view
// ============================================================================

/**
 * @brief Returns the ComponentVersions the **caller's** translation unit
 *        was compiled against.
 *
 * Uses `consteval` to force compile-time evaluation, so the returned
 * struct is folded into the caller's binary as a constant.  A binary
 * built against an older header carries the older values; rebuilding
 * against a newer header updates them.
 */
consteval ComponentVersions compiled_against_here() noexcept
{
    return ComponentVersions{
        static_cast<uint16_t>(PYLABHUB_VERSION_MAJOR),
        static_cast<uint16_t>(PYLABHUB_VERSION_MINOR),
        static_cast<uint16_t>(PYLABHUB_VERSION_ROLLING),
        kShmMajor,           kShmMinor,
        kBrokerProtoMajor,   kBrokerProtoMinor,
        kZmqFrameMajor,      kZmqFrameMinor,
        kScriptApiMajor,     kScriptApiMinor,
        kScriptEngineMajor,  kScriptEngineMinor,
        kConfigMajor,        kConfigMinor,
    };
}

/**
 * @brief Bundle of compile-time ABI expectations, optionally including a
 *        build_id string for strict freshness checks.
 *
 * `build_id` is a pointer to a null-terminated string embedded in the
 * caller binary when `PYLABHUB_HAVE_BUILD_ID` is defined (enabled by
 * CMake in Debug builds and when `PYLABHUB_STRICT_ABI_CHECK=ON`).
 * `nullptr` in Release mode skips the build_id comparison.
 */
struct AbiExpectation
{
    ComponentVersions versions;
    const char       *build_id;   // nullptr → skip build-id check
};

/**
 * @brief Returns the caller's AbiExpectation bundle for use with check_abi().
 *
 * Strict mode (Debug build or `PYLABHUB_STRICT_ABI_CHECK=ON`): includes
 * the compile-time `PYLABHUB_BUILD_ID` string.  Release default: nullptr.
 */
consteval AbiExpectation abi_expected_here() noexcept
{
#if defined(PYLABHUB_HAVE_BUILD_ID) && \
    (defined(PYLABHUB_STRICT_ABI_CHECK) || !defined(NDEBUG))
    return AbiExpectation{compiled_against_here(), PYLABHUB_BUILD_ID};
#else
    return AbiExpectation{compiled_against_here(), nullptr};
#endif
}

// ============================================================================
// Runtime check API
// ============================================================================

/**
 * @brief Result of a compatibility check between a caller's compile-time
 *        expectation and the library's runtime-reported versions.
 */
struct AbiCheckResult
{
    bool        compatible;       ///< false → caller should abort
    std::string message;          ///< human-readable one-liner
    struct MismatchFlags
    {
        bool library        = false;
        bool shm            = false;
        bool broker_proto   = false;
        bool zmq_frame      = false;
        bool script_api     = false;
        bool script_engine  = false;
        bool config         = false;
        bool build_id       = false;
    } major_mismatch;
};

/**
 * @brief Compare caller's compile-time expectation against the runtime
 *        library's version state.
 *
 * Semantics:
 * - Any major-version axis mismatch → `compatible = false` + matching
 *   flag in `major_mismatch`.
 * - Any minor-version mismatch → WARN log, `compatible` unaffected.
 * - `expected_build_id != nullptr` triggers a strict build_id comparison;
 *   mismatch → `compatible = false` + `major_mismatch.build_id`.
 *   `nullptr` skips the build_id check entirely.
 */
PYLABHUB_UTILS_EXPORT
AbiCheckResult check_abi(const ComponentVersions &expected,
                         const char *expected_build_id = nullptr) noexcept;

// ============================================================================
// Query API
// ============================================================================

/**
 * @brief Returns the compile-time component version constants the library
 *        itself was built with.
 */
PYLABHUB_UTILS_EXPORT ComponentVersions current() noexcept;

/**
 * @brief Returns the library's embedded build identifier string
 *        (typically `<git-short-sha>-<build-type>`), or nullptr if the
 *        build was not configured with build-id support.
 */
PYLABHUB_UTILS_EXPORT const char *build_id() noexcept;

/**
 * @brief PEP 440 release version string (e.g., "0.1.0a0").
 */
PYLABHUB_UTILS_EXPORT const char *release_version() noexcept;

/**
 * @brief Bundled Python runtime version (e.g., "3.14.3+20260211").
 */
PYLABHUB_UTILS_EXPORT const char *python_runtime_version() noexcept;

/**
 * @brief Human-readable one-liner for logging.
 */
PYLABHUB_UTILS_EXPORT std::string version_info_string();

/**
 * @brief JSON object for script consumption.  Includes all 7 axes and
 *        (if present) the `build_id` field.  Backward-compat note: the
 *        `wire_*` keys were renamed to `broker_proto_*` on 2026-04-22.
 */
PYLABHUB_UTILS_EXPORT std::string version_info_json();

} // namespace pylabhub::version

// ============================================================================
// C-linkage ABI query (stable symbol for ctypes / dlsym / FFI consumers)
// ============================================================================

/**
 * @brief Returns all version info as a JSON string (C-linkage, ABI-stable).
 *        The returned pointer is valid for the lifetime of the process.
 */
extern "C" PYLABHUB_UTILS_EXPORT const char *pylabhub_abi_info_json(void);
