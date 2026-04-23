/**
 * @file version_registry.cpp
 * @brief Implementation of the centralized version registry + ABI check
 *        facility (HEP-CORE-0026, HEP-CORE-0032).
 */

#include "plh_version_registry.hpp"
#include "plh_platform.hpp"
#include "pylabhub_version.h"  // PYLABHUB_RELEASE_VERSION, PYLABHUB_PYTHON_RUNTIME_VERSION
#include "utils/data_block.hpp" // HEADER_VERSION_MAJOR/MINOR
#include "utils/logger.hpp"

#include <fmt/format.h>

#include <cstring>

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
// check_abi — runtime compatibility assertion
// ============================================================================

AbiCheckResult check_abi(const ComponentVersions &exp,
                         const char *exp_build_id) noexcept
{
    AbiCheckResult r{};
    r.compatible = true;
    const auto cur = current();

    auto check_major = [&](const char *name, auto e, auto c, bool &flag) {
        if (e != c)
        {
            flag = true;
            r.compatible = false;
            r.message += fmt::format("{} major {} != {}; ",
                                     name, static_cast<unsigned>(e),
                                     static_cast<unsigned>(c));
        }
    };
    check_major("library",       exp.library_major,        cur.library_major,
                r.major_mismatch.library);
    check_major("shm",           exp.shm_major,            cur.shm_major,
                r.major_mismatch.shm);
    check_major("broker_proto",  exp.broker_proto_major,   cur.broker_proto_major,
                r.major_mismatch.broker_proto);
    check_major("zmq_frame",     exp.zmq_frame_major,      cur.zmq_frame_major,
                r.major_mismatch.zmq_frame);
    check_major("script_api",    exp.script_api_major,     cur.script_api_major,
                r.major_mismatch.script_api);
    check_major("script_engine", exp.script_engine_major,  cur.script_engine_major,
                r.major_mismatch.script_engine);
    check_major("config",        exp.config_major,         cur.config_major,
                r.major_mismatch.config);

    // Build_id: strict freshness check when non-null.
    if (exp_build_id != nullptr)
    {
        const char *cur_bid = build_id();
        if (cur_bid == nullptr || std::strcmp(exp_build_id, cur_bid) != 0)
        {
            r.major_mismatch.build_id = true;
            r.compatible = false;
            r.message += fmt::format("build_id {} != {}; ",
                                     exp_build_id,
                                     cur_bid ? cur_bid : "(library has no build_id)");
        }
    }

    // Minor-only deltas → WARN.  Safe to call LOGGER_WARN even before
    // LifecycleGuard is up — the logger drops to stderr pre-init.
    auto warn_minor = [&](const char *name, auto e, auto c) {
        if (e != c)
        {
            LOGGER_WARN("ABI check: {} minor {} != {} (additive change; "
                        "caller should re-inspect runtime surface)",
                        name, static_cast<unsigned>(e),
                        static_cast<unsigned>(c));
        }
    };
    warn_minor("library",       exp.library_minor,        cur.library_minor);
    warn_minor("shm",           exp.shm_minor,            cur.shm_minor);
    warn_minor("broker_proto",  exp.broker_proto_minor,   cur.broker_proto_minor);
    warn_minor("zmq_frame",     exp.zmq_frame_minor,      cur.zmq_frame_minor);
    warn_minor("script_api",    exp.script_api_minor,     cur.script_api_minor);
    warn_minor("script_engine", exp.script_engine_minor,  cur.script_engine_minor);
    warn_minor("config",        exp.config_minor,         cur.config_minor);

    if (r.compatible && r.message.empty())
        r.message = "ABI OK";
    else if (!r.message.empty() && r.message.back() == ' ')
        r.message.pop_back(); // trim trailing space after "; "

    return r;
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
