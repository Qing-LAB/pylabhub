#pragma once
/**
 * @file engine_factory.hpp
 * @brief make_engine_from_script_config — construct a ScriptEngine per config.
 *
 * Header-only helper used by every role binary (producer/consumer/processor/
 * plh_role) to turn the "script.type" config field into a fresh
 * @c ScriptEngine subclass instance, wiring type-specific options:
 *
 *   - "native" → NativeEngine; optional `script.checksum`
 *   - "lua"    → LuaEngine
 *   - anything else (including "python") → PythonEngine; optional
 *                 `script.python_venv`
 *
 * Lives in the scripting layer (pylabhub-scripting static lib) because
 * LuaEngine and PythonEngine are only linked by Python-embedding binaries;
 * pylabhub-utils must not depend on libpython.
 */

#include "utils/config/script_config.hpp"
#include "utils/native_engine.hpp"
#include "utils/script_engine.hpp"
#include "lua_engine.hpp"
#include "python_engine.hpp"

#include <memory>

namespace pylabhub::scripting
{

/// Construct a ScriptEngine from the `script` subsection of a role config.
/// Unknown script types fall through to PythonEngine (historical default).
inline std::unique_ptr<ScriptEngine>
make_engine_from_script_config(const config::ScriptConfig &sc)
{
    if (sc.type == "native")
    {
        auto ne = std::make_unique<NativeEngine>();
        if (!sc.checksum.empty())
            ne->set_expected_checksum(sc.checksum);
        return ne;
    }
    if (sc.type == "lua")
    {
        return std::make_unique<LuaEngine>();
    }
    // Default: python (explicit "python" OR unknown type — preserves the
    // prior per-main behaviour).
    auto py = std::make_unique<PythonEngine>();
    if (!sc.python_venv.empty())
        py->set_python_venv(sc.python_venv);
    return py;
}

} // namespace pylabhub::scripting
