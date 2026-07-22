#pragma once
/**
 * @file script_config.hpp
 * @brief ScriptConfig — categorical config for script engine selection and paths.
 *
 * Parsed from the "script" JSON section + top-level "python_venv" and
 * "stop_on_script_error". Single source of truth for script.type validation.
 */

#include "utils/json_fwd.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct ScriptConfig
{
    std::string type{"python"};       ///< "python", "lua", "native", or "none" (hub-only: no engine)
    std::string path{"."};            ///< Parent dir of the script/<type>/ package
    std::string python_venv;          ///< venv name (Python only), empty = base env
    std::string checksum;             ///< BLAKE2b-256 hex of native .so (native only)
    bool type_explicit{false};        ///< true when "type" was present in JSON
    bool stop_on_script_error{false}; ///< Fatal on script exception in callback.

    /// True iff this config should construct and run a script engine.  The
    /// single, explicit "run a script?" signal shared by the hub main's
    /// interpreter eager-load and HubHost's runner construction.  `type ==
    /// "none"` (hub-only: run as a pure broker) OR an empty `path` (legacy
    /// script-disabled hub) both mean "no engine".  Roles ALWAYS run an engine
    /// — config validation rejects `none`/empty-path for roles — so for a role
    /// this is always true.
    [[nodiscard]] bool runs_script_engine() const noexcept
    {
        return type != "none" && !path.empty();
    }

    /// Release the engine's global interpreter lock during the worker
    /// loop's idle waits (queue I/O wait, deadline sleep, hub event
    /// wait).  Default: false — preserve the hot-path no-overhead
    /// behaviour that has shipped historically.
    ///
    /// **Applies only when the engine has a single global interpreter
    /// lock** (i.e. `ScriptEngine::supports_multi_state() == false`).
    /// The PythonEngine reads this flag and reports it via
    /// `release_global_lock_during_wait()`; LuaEngine and NativeEngine
    /// have no global lock to release, so they always report false
    /// regardless of this setting.
    ///
    /// **Use case (the only one):** Python scripts that spawn
    /// cooperative sub-threads — e.g. a hub script running a Flask
    /// server on a `threading.Thread`, or a role script using
    /// `asyncio` in a side thread.  Without this flag, the worker
    /// thread holds the GIL across `wait_for_incoming` / queue reads /
    /// deadline sleeps, starving Python sub-threads on the same
    /// interpreter.  With this flag, the GIL is released across those
    /// waits and the sub-threads make progress.
    ///
    /// **Cost when enabled:** one PyEval_SaveThread + PyEval_RestoreThread
    /// pair per loop iteration's idle wait.  Measurable for
    /// >1 kHz tick loops; negligible for typical 10–100 Hz roles.
    /// **Cost when disabled (default):** zero — the optional guard
    /// inside the loop is constant-folded away.
    ///
    /// ──────────────────────────────────────────────────────────────
    /// **WARNING — script contract for opting in.**
    /// ──────────────────────────────────────────────────────────────
    ///
    /// CPython's GIL reacquire (`PyEval_RestoreThread`, called by the
    /// worker's loop frame on its way out of the idle-wait scope) is
    /// not a hard wait: CPython 3.10+ wakes the would-be acquirer
    /// every `sys.setswitchinterval()` (default **5 ms**) and signals
    /// the current GIL holder (`_PY_GIL_DROP_REQUEST_BIT` on the
    /// holder's eval breaker).  Pure-Python code observes the eval
    /// breaker at every bytecode boundary (~100 bytecodes since 3.10)
    /// and yields the GIL.  In practice:
    ///
    ///   - Pure-Python `while True: pass`               → yields ~5 ms
    ///   - `time.sleep(...)`                            → yields immediately
    ///   - Most NumPy / SciPy / pandas ops              → yield (`with nogil:`)
    ///   - **A C extension that does NOT release GIL**  → wedges reacquire
    ///
    /// **The only failure mode is a script-spawned C extension that
    /// holds the GIL without yielding.**  Examples that can wedge:
    /// long native NumPy/SciPy ops without `with nogil:`, custom C
    /// modules with extended pure-C paths, `time.sleep` from a C
    /// library that does not drop the GIL.  Pure Python code in
    /// sub-threads cannot wedge the worker — it always yields.
    ///
    /// If a wedge occurs, the worker thread will block inside the
    /// optional's destructor on `PyEval_RestoreThread`.  The bounded-
    /// join in `EngineHost::shutdown_()` (HEP-CORE-0011 §"ThreadManager"
    /// → "Bounded Shutdown Join") will detect this within the per-
    /// thread join timeout (default 5 s), DETACH the worker thread,
    /// emit a CRITICAL log naming this flag as the likely cause, and
    /// allow the parent process to continue clean teardown.  The
    /// detached worker is leaked; some engine-owned resources (Python
    /// objects, sockets) may leak with it until process exit.
    ///
    /// **Recommendation when opting in:** review every C extension
    /// reachable from the script's sub-threads.  Verify each either
    /// (a) is pure Python, (b) wraps its native work in `with nogil:`,
    /// or (c) returns within a few hundred ms.  Test the shutdown
    /// path under representative workload before deployment.
    bool release_global_lock_during_wait{false};
};

/// Platform-specific shared library extension.
inline const char *native_lib_extension() noexcept
{
#if defined(_WIN32) || defined(_WIN64)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

/// Resolve a native engine library path from the config-specified path.
///
/// Follows the same convention as Python/Lua: script/<type>/<entry>.
/// For native, this is: <script.path>/script/native/<filename>
///
/// Search order:
/// 1. Exact path (if file exists)
/// 2. path + platform extension (if no extension given)
/// 3. script_dir/script/native/<filename> (structured, parallel to python/lua)
/// 4. script_dir/script/native/<filename> + platform extension
///
/// @param configured_path  The "script.path" value from config (resolved).
/// @param filename         Library filename (e.g., "libmy_producer" or "libmy_producer.so").
/// @return Resolved absolute path, or empty if not found.
inline std::filesystem::path resolve_native_library(const std::string &configured_path,
                                                    const std::string &filename)
{
    namespace fs = std::filesystem;
    const auto ext = native_lib_extension();

    // 1. Exact path (filename is a full path).
    fs::path p(filename);
    if (p.is_absolute() && fs::exists(p) && fs::is_regular_file(p))
        return fs::weakly_canonical(p);

    // 2. Structured: <script.path>/script/native/<filename>
    fs::path script_dir(configured_path);
    fs::path structured = script_dir / "script" / "native" / filename;
    if (fs::exists(structured) && fs::is_regular_file(structured))
        return fs::weakly_canonical(structured);

    // 3. Append platform extension.
    if (structured.extension().empty())
    {
        fs::path with_ext = structured;
        with_ext += ext;
        if (fs::exists(with_ext) && fs::is_regular_file(with_ext))
            return fs::weakly_canonical(with_ext);
    }

    // 4. Direct relative to script.path.
    fs::path direct = script_dir / filename;
    if (fs::exists(direct) && fs::is_regular_file(direct))
        return fs::weakly_canonical(direct);

    if (direct.extension().empty())
    {
        fs::path with_ext = direct;
        with_ext += ext;
        if (fs::exists(with_ext) && fs::is_regular_file(with_ext))
            return fs::weakly_canonical(with_ext);
    }

    return {}; // not found
}

/// Parse the "script" section from a JSON config object.
/// Validates type ∈ {"python", "lua", "native"}.
/// Also reads top-level "python_venv".
/// @param j      Root JSON object.
/// @param base   Role directory base path (for resolving relative script.path).
///               Pass empty path to skip resolution (from_json_file mode).
/// @param tag    Context tag for error messages (e.g., "Producer config").
/// @param script_optional  When true (hub), `type:"none"` and an empty `path`
///        are accepted (run as a pure broker, no engine).  When false (roles),
///        the script is mandatory: `type` must be a real engine and `path` must
///        be non-empty, else construction fails fast at config time.
inline ScriptConfig parse_script_config(const nlohmann::json &j, const std::filesystem::path &base,
                                        const char *tag, bool script_optional = false)
{
    namespace fs = std::filesystem;
    ScriptConfig sc;

    if (j.contains("script") && j["script"].is_object())
    {
        const auto &s = j["script"];
        // Reject unknown nested keys so typos like "pahh" instead of
        // "path" fail loudly instead of silently defaulting.
        for (auto it = s.begin(); it != s.end(); ++it)
        {
            const auto &k = it.key();
            if (k != "type" && k != "path" && k != "checksum" &&
                k != "release_global_lock_during_wait")
                throw std::runtime_error(std::string(tag) + ": unknown config key 'script." + k +
                                         "'");
        }
        sc.type_explicit = s.contains("type");
        sc.type = s.value("type", std::string{"python"});
        const bool type_ok = sc.type == "python" || sc.type == "lua" || sc.type == "native" ||
                             (script_optional && sc.type == "none");
        if (!type_ok)
            throw std::invalid_argument(
                std::string(tag) + ": script.type must be \"python\", \"lua\", or \"native\"" +
                (script_optional ? " (or \"none\" for a script-less hub)" : "") + ", got: \"" +
                sc.type + "\"");
        sc.path = s.value("path", std::string{"."});
        sc.checksum = s.value("checksum", std::string{});
        sc.release_global_lock_during_wait = s.value("release_global_lock_during_wait", false);
    }

    sc.python_venv = j.value("python_venv", std::string{});
    sc.stop_on_script_error = j.value("stop_on_script_error", false);

    // Resolve relative script.path against the role directory base.
    if (!base.empty() && !sc.path.empty() && !fs::path(sc.path).is_absolute())
        sc.path = fs::weakly_canonical(base / sc.path).string();

    // Roles (script_optional == false) always run an engine — reject a path-less
    // role at config time.  A real-engine role with no script has nothing to
    // load; catching it here fails fast and clearly instead of paying
    // interpreter init and dying at load_script (or, for Python, hard-panicking
    // if the main's eager-load were ever skipped).  The hub is exempt: an empty
    // path (like type:"none") means "run as a pure broker".
    if (!script_optional && sc.path.empty())
        throw std::invalid_argument(
            std::string(tag) +
            ": script.path must be non-empty — a role always runs a script engine "
            "(python/lua/native) and a path-less role has no script to load");

    return sc;
}

} // namespace pylabhub::config
