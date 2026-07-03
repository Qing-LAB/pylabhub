/**
 * @file version_registry.cpp
 * @brief Implementation of the centralized version registry + ABI check
 *        facility (HEP-CORE-0026, HEP-CORE-0032).
 */

#include "plh_version_registry.hpp"
#include "plh_platform.hpp"
#include "pylabhub_version.h"  // PYLABHUB_RELEASE_VERSION, PYLABHUB_PYTHON_RUNTIME_VERSION
#include "utils/data_block.hpp" // HEADER_VERSION_MAJOR/MINOR
#include "utils/json_fwd.hpp"  // full nlohmann::json for to_json_object/from_json_object
#include "utils/native_engine_api.h"  // PLH_COMPONENT_* C-visible mirrors

#include <fmt/format.h>

#include <cstdio>
#include <cstring>
#include <limits>       // std::numeric_limits — from_json_object range check
#include <stdexcept>
#include <vector>

// ============================================================================
// Pin the header's SHM constants against data_block.hpp's authoritative
// values.  A drift in either location is a compile-time error.
// ============================================================================

static_assert(
    pylabhub::version::kShmMajor == pylabhub::hub::detail::HEADER_VERSION_MAJOR,
    "plh_version_registry.hpp: kShmMajor drifted from "
    "data_block.hpp HEADER_VERSION_MAJOR");
static_assert(
    pylabhub::version::kShmMinor == pylabhub::hub::detail::HEADER_VERSION_MINOR,
    "plh_version_registry.hpp: kShmMinor drifted from "
    "data_block.hpp HEADER_VERSION_MINOR");

// Pin the C-visible PLH_COMPONENT_* #defines (native_engine_api.h) to
// the C++ `inline constexpr` values (plh_version_registry.hpp).  The
// two locations exist because C plugins cannot include the C++ header;
// a drift is a compile-time error.
static_assert(PLH_COMPONENT_SHM_MAJOR           == pylabhub::version::kShmMajor,
              "PLH_COMPONENT_SHM_MAJOR drift");
static_assert(PLH_COMPONENT_SHM_MINOR           == pylabhub::version::kShmMinor,
              "PLH_COMPONENT_SHM_MINOR drift");
static_assert(PLH_COMPONENT_BROKER_PROTO_MAJOR  == pylabhub::version::kBrokerProtoMajor,
              "PLH_COMPONENT_BROKER_PROTO_MAJOR drift");
static_assert(PLH_COMPONENT_BROKER_PROTO_MINOR  == pylabhub::version::kBrokerProtoMinor,
              "PLH_COMPONENT_BROKER_PROTO_MINOR drift");
static_assert(PLH_COMPONENT_ZMQ_FRAME_MAJOR     == pylabhub::version::kZmqFrameMajor,
              "PLH_COMPONENT_ZMQ_FRAME_MAJOR drift");
static_assert(PLH_COMPONENT_ZMQ_FRAME_MINOR     == pylabhub::version::kZmqFrameMinor,
              "PLH_COMPONENT_ZMQ_FRAME_MINOR drift");
static_assert(PLH_COMPONENT_SCRIPT_API_MAJOR    == pylabhub::version::kScriptApiMajor,
              "PLH_COMPONENT_SCRIPT_API_MAJOR drift");
static_assert(PLH_COMPONENT_SCRIPT_API_MINOR    == pylabhub::version::kScriptApiMinor,
              "PLH_COMPONENT_SCRIPT_API_MINOR drift");
static_assert(PLH_COMPONENT_SCRIPT_ENGINE_MAJOR == pylabhub::version::kScriptEngineMajor,
              "PLH_COMPONENT_SCRIPT_ENGINE_MAJOR drift");
static_assert(PLH_COMPONENT_SCRIPT_ENGINE_MINOR == pylabhub::version::kScriptEngineMinor,
              "PLH_COMPONENT_SCRIPT_ENGINE_MINOR drift");
static_assert(PLH_COMPONENT_CONFIG_MAJOR        == pylabhub::version::kConfigMajor,
              "PLH_COMPONENT_CONFIG_MAJOR drift");
static_assert(PLH_COMPONENT_CONFIG_MINOR        == pylabhub::version::kConfigMinor,
              "PLH_COMPONENT_CONFIG_MINOR drift");

namespace pylabhub::version
{

ComponentVersions current() noexcept
{
    return ComponentVersions{
        static_cast<uint16_t>(platform::get_version_major()),
        static_cast<uint16_t>(platform::get_version_minor()),
        static_cast<uint16_t>(platform::get_version_rolling()),
        kShmMajor,             kShmMinor,
        kBrokerProtoMajor,     kBrokerProtoMinor,
        kZmqFrameMajor,        kZmqFrameMinor,
        kScriptApiMajor,       kScriptApiMinor,
        kScriptEngineMajor,    kScriptEngineMinor,
        kConfigMajor,          kConfigMinor,
    };
}

const char *build_id() noexcept
{
#ifdef PYLABHUB_HAVE_BUILD_ID
    return PYLABHUB_BUILD_ID;
#else
    return nullptr;
#endif
}

const char *release_version() noexcept
{
    return PYLABHUB_RELEASE_VERSION;
}

const char *python_runtime_version() noexcept
{
    return PYLABHUB_PYTHON_RUNTIME_VERSION;
}

std::string version_info_string()
{
    const auto v = current();
    const char *bid = build_id();
    return fmt::format(
        "pylabhub {} (lib={}.{}.{}, shm={}.{}, broker={}.{}, zmq={}.{}, "
        "script={}.{}, engine={}.{}, config={}.{}, python_rt={}{}{})",
        PYLABHUB_RELEASE_VERSION,
        v.library_major, v.library_minor, v.library_rolling,
        v.shm_major, v.shm_minor,
        v.broker_proto_major, v.broker_proto_minor,
        v.zmq_frame_major, v.zmq_frame_minor,
        v.script_api_major, v.script_api_minor,
        v.script_engine_major, v.script_engine_minor,
        v.config_major, v.config_minor,
        PYLABHUB_PYTHON_RUNTIME_VERSION,
        bid ? ", build=" : "",
        bid ? bid : "");
}

std::string version_info_json()
{
    const auto v = current();
    const char *bid = build_id();
    return fmt::format(
        R"({{"release":"{}","library":"{}.{}.{}","python_runtime":"{}",)"
        R"("shm_major":{},"shm_minor":{},)"
        R"("broker_proto_major":{},"broker_proto_minor":{},)"
        R"("zmq_frame_major":{},"zmq_frame_minor":{},)"
        R"("script_api_major":{},"script_api_minor":{},)"
        R"("script_engine_major":{},"script_engine_minor":{},)"
        R"("config_major":{},"config_minor":{}{})"
        R"(}})",
        PYLABHUB_RELEASE_VERSION,
        v.library_major, v.library_minor, v.library_rolling,
        PYLABHUB_PYTHON_RUNTIME_VERSION,
        v.shm_major, v.shm_minor,
        v.broker_proto_major, v.broker_proto_minor,
        v.zmq_frame_major, v.zmq_frame_minor,
        v.script_api_major, v.script_api_minor,
        v.script_engine_major, v.script_engine_minor,
        v.config_major, v.config_minor,
        bid ? fmt::format(R"(,"build_id":"{}")", bid) : std::string{});
}

// ============================================================================
// Pure comparator — no side effects, no I/O.
//
// Shared by:
//   - `check_abi`             (startup self-check; caller's exp vs mine)
//   - `verify_peer_versions`  (peer verification; peer's versions vs mine)
//
// Two public entry points, one internal comparator.  See
// HEP-CORE-0032 §8.5 for the taxonomy this implements.
// ============================================================================

namespace detail
{

/// Compare `remote` (peer or startup expectation) against local
/// `current()`, filling in `major_mismatch` flags for MAJOR-axis
/// differences and appending `minor_notes` for MINOR differences.
/// Neither log output nor stderr is produced here; callers decide
/// where to sink the verdict.
struct RawVerdict
{
    AbiCheckResult              result;
    std::vector<std::string>    minor_notes;   ///< one entry per drifted minor axis
    std::vector<const char *>   major_axes;    ///< names of MAJOR-mismatched axes
    std::vector<const char *>   minor_axes;    ///< names of MINOR-mismatched axes
};

static RawVerdict compute_verdict(const ComponentVersions &remote,
                                  const char *remote_build_id) noexcept
{
    RawVerdict v{};
    v.result.compatible = true;
    const auto cur = current();

    auto check_major = [&](const char *name, auto rem, auto loc, bool &flag) {
        if (rem != loc)
        {
            flag                    = true;
            v.result.compatible     = false;
            v.result.message += fmt::format("{} major {} != {}; ",
                                            name, static_cast<unsigned>(rem),
                                            static_cast<unsigned>(loc));
            v.major_axes.push_back(name);
        }
    };
    check_major("library",       remote.library_major,        cur.library_major,
                v.result.major_mismatch.library);
    check_major("shm",           remote.shm_major,            cur.shm_major,
                v.result.major_mismatch.shm);
    check_major("broker_proto",  remote.broker_proto_major,   cur.broker_proto_major,
                v.result.major_mismatch.broker_proto);
    check_major("zmq_frame",     remote.zmq_frame_major,      cur.zmq_frame_major,
                v.result.major_mismatch.zmq_frame);
    check_major("script_api",    remote.script_api_major,     cur.script_api_major,
                v.result.major_mismatch.script_api);
    check_major("script_engine", remote.script_engine_major,  cur.script_engine_major,
                v.result.major_mismatch.script_engine);
    check_major("config",        remote.config_major,         cur.config_major,
                v.result.major_mismatch.config);

    // build_id: strict freshness check when non-null.
    if (remote_build_id != nullptr)
    {
        const char *cur_bid = build_id();
        if (cur_bid == nullptr || std::strcmp(remote_build_id, cur_bid) != 0)
        {
            v.result.major_mismatch.build_id = true;
            v.result.compatible              = false;
            // Fix 2026-07-03 code review Finding #13: keep the
            // side-specific hint ("this library"/"library has no
            // build_id") so operators reading the diagnostic can tell
            // WHICH side lacks build_id without inspecting both
            // binaries.  Post-refactor the shorter "(no build_id)"
            // ambiguously read as either side.
            v.result.message += fmt::format("build_id {} != {}; ",
                                            remote_build_id,
                                            cur_bid ? cur_bid
                                                     : "(library has no build_id)");
            v.major_axes.push_back("build_id");
        }
    }

    auto note_minor = [&](const char *name, auto rem, auto loc, bool &flag) {
        if (rem != loc)
        {
            flag = true;
            v.minor_notes.push_back(fmt::format("{} minor {} != {}",
                                                name,
                                                static_cast<unsigned>(rem),
                                                static_cast<unsigned>(loc)));
            v.minor_axes.push_back(name);
        }
    };
    note_minor("library",       remote.library_minor,        cur.library_minor,
                v.result.minor_mismatch.library);
    note_minor("shm",           remote.shm_minor,            cur.shm_minor,
                v.result.minor_mismatch.shm);
    note_minor("broker_proto",  remote.broker_proto_minor,   cur.broker_proto_minor,
                v.result.minor_mismatch.broker_proto);
    note_minor("zmq_frame",     remote.zmq_frame_minor,      cur.zmq_frame_minor,
                v.result.minor_mismatch.zmq_frame);
    note_minor("script_api",    remote.script_api_minor,     cur.script_api_minor,
                v.result.minor_mismatch.script_api);
    note_minor("script_engine", remote.script_engine_minor,  cur.script_engine_minor,
                v.result.minor_mismatch.script_engine);
    note_minor("config",        remote.config_minor,         cur.config_minor,
                v.result.minor_mismatch.config);

    if (v.result.compatible && v.result.message.empty())
        v.result.message = "ABI OK";
    else if (!v.result.message.empty() && v.result.message.back() == ' ')
        v.result.message.pop_back();  // trim trailing space after "; "

    return v;
}

} // namespace detail

// ============================================================================
// check_abi — startup self-check.  Emits minor warnings to stderr
// because it runs BEFORE LifecycleGuard (Logger state is still
// Uninitialized; LOGGER_WARN would trigger PLH_PANIC per
// logger.cpp:67).  Direct stderr keeps the WARN visible without a
// lifecycle precondition.
// ============================================================================

AbiCheckResult check_abi(const ComponentVersions &exp,
                         const char *exp_build_id) noexcept
{
    auto v = detail::compute_verdict(exp, exp_build_id);
    // 2026-07-03 code review Finding #9 — emit ONE stderr WARN line
    // per drifted minor axis rather than one joined line for all of
    // them.  Log-aggregators that count distinct WARN lines to gauge
    // drift severity (fire alert on ≥2 axes) previously miscounted a
    // multi-axis drift as a single event.
    for (const auto &note : v.minor_notes)
    {
        std::fprintf(stderr,
                      "[pylabhub] ABI check WARN: %s "
                      "(additive change; caller should re-inspect runtime surface)\n",
                      note.c_str());
    }
    return v.result;
}

// ============================================================================
// verify_peer_versions — peer verification on wire ingest.  Pure: no
// I/O, no logger call, no stderr write.  Caller (broker's REG_REQ
// handler or role's REG_ACK handler) emits the log line via the
// Logger per HEP-CORE-0032 §8.6.
// ============================================================================

AbiCheckResult verify_peer_versions(const ComponentVersions &peer_versions,
                                    const char *peer_build_id) noexcept
{
    return detail::compute_verdict(peer_versions, peer_build_id).result;
}

// ============================================================================
// classify_peer_verdict — HEP-CORE-0032 §8.6 verdict classification.
// Shared helper for broker-side + role-side observability so the two
// stay in sync on taxonomy changes.  (2026-07-03 code review
// Finding #14.)
// ============================================================================

AbiPeerVerdict classify_peer_verdict(const AbiCheckResult &v)
{
    AbiPeerVerdict out;

    // Append helpers for axis lists.
    auto append = [](std::string &dst, bool flag, const char *name) {
        if (!flag) return;
        if (!dst.empty()) dst += ",";
        dst += name;
    };
    append(out.major_axes, v.major_mismatch.library,       "library");
    append(out.major_axes, v.major_mismatch.shm,           "shm");
    append(out.major_axes, v.major_mismatch.broker_proto,  "broker_proto");
    append(out.major_axes, v.major_mismatch.zmq_frame,     "zmq_frame");
    append(out.major_axes, v.major_mismatch.script_api,    "script_api");
    append(out.major_axes, v.major_mismatch.script_engine, "script_engine");
    append(out.major_axes, v.major_mismatch.config,        "config");
    append(out.major_axes, v.major_mismatch.build_id,      "build_id");

    append(out.minor_axes, v.minor_mismatch.library,       "library");
    append(out.minor_axes, v.minor_mismatch.shm,           "shm");
    append(out.minor_axes, v.minor_mismatch.broker_proto,  "broker_proto");
    append(out.minor_axes, v.minor_mismatch.zmq_frame,     "zmq_frame");
    append(out.minor_axes, v.minor_mismatch.script_api,    "script_api");
    append(out.minor_axes, v.minor_mismatch.script_engine, "script_engine");
    append(out.minor_axes, v.minor_mismatch.config,        "config");

    if (!v.compatible)
    {
        // BUILD_ONLY iff build_id is the ONLY major flag set.
        const bool only_build_id =
            v.major_mismatch.build_id &&
            !v.major_mismatch.library &&
            !v.major_mismatch.shm &&
            !v.major_mismatch.broker_proto &&
            !v.major_mismatch.zmq_frame &&
            !v.major_mismatch.script_api &&
            !v.major_mismatch.script_engine &&
            !v.major_mismatch.config;
        out.kind = only_build_id ? AbiPeerVerdict::Kind::BuildOnly
                                  : AbiPeerVerdict::Kind::MajorMismatch;
    }
    else if (!out.minor_axes.empty())
    {
        out.kind = AbiPeerVerdict::Kind::MinorMismatch;
    }
    else
    {
        out.kind = AbiPeerVerdict::Kind::Ok;
    }
    return out;
}

// ============================================================================
// JSON serialization for wire binding (HEP-CORE-0032 §8.2)
// ============================================================================

nlohmann::json to_json_object(const ComponentVersions &v)
{
    return nlohmann::json{
        {"library_major",       v.library_major},
        {"library_minor",       v.library_minor},
        {"library_rolling",     v.library_rolling},
        {"shm_major",           v.shm_major},
        {"shm_minor",           v.shm_minor},
        {"broker_proto_major",  v.broker_proto_major},
        {"broker_proto_minor",  v.broker_proto_minor},
        {"zmq_frame_major",     v.zmq_frame_major},
        {"zmq_frame_minor",     v.zmq_frame_minor},
        {"script_api_major",    v.script_api_major},
        {"script_api_minor",    v.script_api_minor},
        {"script_engine_major", v.script_engine_major},
        {"script_engine_minor", v.script_engine_minor},
        {"config_major",        v.config_major},
        {"config_minor",        v.config_minor},
    };
}

ComponentVersions from_json_object(const nlohmann::json &j)
{
    if (!j.is_object())
    {
        throw std::invalid_argument(
            "abi_fingerprint: expected JSON object");
    }

    // Small helper: fetch a REQUIRED integer axis field, range-checked
    // against the target type's max.  A missing field, wrong type, or
    // out-of-range value is treated as INVALID_REQUEST per
    // HEP-CORE-0032 §8.7 — wire-shape invariant, not a silently-truncated
    // value (an unchecked static_cast<uint8_t>(256) → 0 would let a peer
    // bypass strict-mode reject by wrapping around to the local axis
    // value; see 2026-07-03 code review Finding #1).
    auto req_axis_u8 = [&](const char *key) -> uint8_t {
        auto it = j.find(key);
        if (it == j.end() || !it->is_number_unsigned())
        {
            throw std::invalid_argument(
                fmt::format("abi_fingerprint: missing or non-unsigned "
                             "axis field '{}'", key));
        }
        const auto raw = it->get<uint64_t>();
        if (raw > std::numeric_limits<uint8_t>::max())
        {
            throw std::invalid_argument(
                fmt::format("abi_fingerprint: axis '{}' value {} exceeds "
                             "uint8_t range", key, raw));
        }
        return static_cast<uint8_t>(raw);
    };
    auto req_axis_u16 = [&](const char *key) -> uint16_t {
        auto it = j.find(key);
        if (it == j.end() || !it->is_number_unsigned())
        {
            throw std::invalid_argument(
                fmt::format("abi_fingerprint: missing or non-unsigned "
                             "axis field '{}'", key));
        }
        const auto raw = it->get<uint64_t>();
        if (raw > std::numeric_limits<uint16_t>::max())
        {
            throw std::invalid_argument(
                fmt::format("abi_fingerprint: axis '{}' value {} exceeds "
                             "uint16_t range", key, raw));
        }
        return static_cast<uint16_t>(raw);
    };

    ComponentVersions v{};
    v.library_major       = req_axis_u16("library_major");
    v.library_minor       = req_axis_u16("library_minor");
    v.library_rolling     = req_axis_u16("library_rolling");
    v.shm_major           = req_axis_u8("shm_major");
    v.shm_minor           = req_axis_u8("shm_minor");
    v.broker_proto_major  = req_axis_u8("broker_proto_major");
    v.broker_proto_minor  = req_axis_u8("broker_proto_minor");
    v.zmq_frame_major     = req_axis_u8("zmq_frame_major");
    v.zmq_frame_minor     = req_axis_u8("zmq_frame_minor");
    v.script_api_major    = req_axis_u8("script_api_major");
    v.script_api_minor    = req_axis_u8("script_api_minor");
    v.script_engine_major = req_axis_u8("script_engine_major");
    v.script_engine_minor = req_axis_u8("script_engine_minor");
    v.config_major        = req_axis_u8("config_major");
    v.config_minor        = req_axis_u8("config_minor");
    // Unknown / extra fields intentionally ignored — MINOR-bump
    // forward-compat contract per HEP-CORE-0032 §8.3.2.
    return v;
}

} // namespace pylabhub::version

// ============================================================================
// C-linkage ABI query — stable symbol for ctypes / dlsym / FFI consumers.
// ============================================================================

extern "C" const char *pylabhub_abi_info_json(void)
{
    static const std::string s = pylabhub::version::version_info_json();
    return s.c_str();
}
