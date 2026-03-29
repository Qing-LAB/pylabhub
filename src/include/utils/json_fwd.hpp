#pragma once
// utils/json_fwd.hpp — Project-level JSON abstraction layer
//
// All source files include this header instead of <nlohmann/json.hpp>
// directly.  If the project ever switches JSON libraries, only this
// file (and the .cpp files that use library-specific API) need to change.
//
// ## Null-as-absent convention
//
// JSON has no comment syntax. To allow users to "comment out" a config
// parameter without deleting it, we treat `null` values as absent:
//
//   "target_period_ms": null     ← treated as if the key were not present
//   "target_period_ms": 10.0    ← present and set
//
// All config parsers should use config_has() and config_value() instead
// of raw j.contains() and j.value() to honour this convention.

#include <nlohmann/json.hpp>

namespace pylabhub::config
{

/// Check if a JSON key is present and not null.
/// A null value means "commented out" — treated as absent.
inline bool config_has(const nlohmann::json &j, const char *key)
{
    return j.contains(key) && !j[key].is_null();
}

/// Get a JSON value with default, treating null as absent.
template <typename T>
T config_value(const nlohmann::json &j, const char *key, const T &default_val)
{
    if (!j.contains(key) || j[key].is_null())
        return default_val;
    return j[key].get<T>();
}

} // namespace pylabhub::config
