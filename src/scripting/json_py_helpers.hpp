#pragma once
/**
 * @file json_py_helpers.hpp
 * @brief Shared nlohmann::json ↔ pybind11 py::object converters.
 *
 * Internal to `pylabhub-scripting`.  Hosted in the private header dir so
 * every binding in the script engine layer can use the same recursive
 * walker instead of round-tripping through Python's `json.loads` /
 * `json.dumps`.
 *
 * Callers (post-S5 extraction):
 *   - `python_engine.cpp` — dispatch hot path: `invoke(name, args)`
 *     kwargs unpack + `eval` return value + `build_messages_list_*`
 *     event-detail conversion (was always on the fast path; moving the
 *     definition here doesn't change call shape).
 *   - `producer_api.cpp` / `consumer_api.cpp` / `processor_api.cpp` —
 *     `metrics()` returns py::dict.
 *   - `hub_api_python.cpp` — `pylabhub_hub.HubAPI.metrics()` (Phase 7
 *     D4.1).
 *
 * ## Semantics vs. `json.loads(dump())` round-trip
 *
 * The fast path differs from the slow string-round-trip in three rows.
 * All three favor "preserve information" over "match json.loads
 * strictness exactly".  Confirm the divergence is acceptable before
 * adopting in a new caller; for `metrics()` it is, since metrics
 * payloads are well-defined integers / strings / nested dicts with no
 * NaN/Inf and no circular refs.
 *
 *   | Case            | json.loads(dump())            | json_to_py        |
 *   |-----------------|-------------------------------|-------------------|
 *   | NaN / Inf       | `nlohmann::json::dump` emits  | py::float_(NaN/Inf
 *   |                 | "null"; loads as None         | preserved)        |
 *   | Deep recursion  | Python RecursionError         | "<recursion       |
 *   |                 |                               | limit>" string    |
 *   |                 |                               | sentinel          |
 *   | bool vs int     | Python distinguishes          | Same — bool       |
 *   |                 |                               | checked BEFORE    |
 *   |                 |                               | int (isinstance   |
 *   |                 |                               | (True, int) is    |
 *   |                 |                               | True in Python)   |
 *
 * Rows that match (silent verifications): integer width (uint64 max
 * round-trips fine — Python ints are arbitrary precision both ways),
 * float precision (nlohmann emits with shortest-round-trip; pybind11's
 * py::float_ is double, identical), object key order (both preserve
 * insertion order on Python 3.7+), array order (stable), null
 * (nullptr → py::none() ⇄ json null).
 *
 * ## Why namespace::detail
 *
 * Private API within the script engine layer.  No public header re-
 * exports these — adding a new caller is fine, exposing to user code
 * (or to pylabhub-utils) requires a deliberate API decision so we
 * don't accidentally bake the divergence rows above into a stable
 * surface.
 */

#include "utils/script_engine.hpp"   // kScriptMaxRecursionDepth

#include "utils/role_api_base.hpp"          // AllowedPeer

#include <pybind11/pybind11.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pylabhub::scripting::detail
{

namespace py = pybind11;

/// Convert nlohmann::json → py::object (dict / list / scalar).
/// Recursion-bounded by `kScriptMaxRecursionDepth`; over-deep nodes
/// substitute `"<recursion limit>"` instead of raising.
inline py::object json_to_py(const nlohmann::json &val, int depth = 0)
{
    if (depth > kScriptMaxRecursionDepth)
        return py::str("<recursion limit>");
    if (val.is_string())
        return py::str(val.get<std::string>());
    if (val.is_boolean())
        return py::bool_(val.get<bool>());
    if (val.is_number_unsigned())
        return py::int_(val.get<std::uint64_t>());
    if (val.is_number_integer())
        return py::int_(val.get<std::int64_t>());
    if (val.is_number_float())
        return py::float_(val.get<double>());
    if (val.is_object())
    {
        py::dict d;
        for (auto &[k, v] : val.items())
            d[py::str(k)] = json_to_py(v, depth + 1);
        return std::move(d);
    }
    if (val.is_array())
    {
        py::list l;
        for (auto &elem : val)
            l.append(json_to_py(elem, depth + 1));
        return std::move(l);
    }
    if (val.is_null())
        return py::none();
    return py::str(val.dump());
}

/// Convert py::object → nlohmann::json.  bool MUST be checked before
/// int — `isinstance(True, int)` is True in Python.  Falls back to
/// repr-as-string for unknown types (callable, custom class, ...).
/// Recursion-bounded by `kScriptMaxRecursionDepth`.
inline nlohmann::json py_to_json(const py::object &obj, int depth = 0)
{
    if (depth > kScriptMaxRecursionDepth)
        return "<recursion limit>";
    if (obj.is_none())
        return nullptr;
    if (py::isinstance<py::bool_>(obj))
        return obj.cast<bool>();
    if (py::isinstance<py::int_>(obj))
        return obj.cast<std::int64_t>();
    if (py::isinstance<py::float_>(obj))
        return obj.cast<double>();
    if (py::isinstance<py::str>(obj))
        return obj.cast<std::string>();
    if (py::isinstance<py::dict>(obj))
    {
        nlohmann::json j = nlohmann::json::object();
        for (auto &item : obj.cast<py::dict>())
            j[item.first.cast<std::string>()] =
                py_to_json(item.second.cast<py::object>(), depth + 1);
        return j;
    }
    if (py::isinstance<py::list>(obj))
    {
        nlohmann::json j = nlohmann::json::array();
        for (auto &elem : obj.cast<py::list>())
            j.push_back(py_to_json(elem.cast<py::object>(), depth + 1));
        return j;
    }
    if (py::isinstance<py::tuple>(obj))
    {
        nlohmann::json j = nlohmann::json::array();
        for (auto &elem : obj.cast<py::tuple>())
            j.push_back(py_to_json(elem.cast<py::object>(), depth + 1));
        return j;
    }
    return py::str(obj).cast<std::string>();
}

/// Convert a vector of AllowedPeer records → py::list of py::dicts
/// shaped `[{"role_uid": str, "pubkey": str}, ...]`.  Shared by the
/// six `allowed_peers` / `producers` bindings on
/// ProducerAPI / ConsumerAPI / ProcessorAPI (HEP-CORE-0036 §I11
/// polling surfaces; HEP-CORE-0011 §"Cross-Engine Surface Parity"
/// Read-only observation surface principle).  Single source of
/// truth for the AllowedPeer wire shape on the Python side.
inline py::list peer_list_to_py(const std::vector<AllowedPeer> &peers)
{
    py::list out;
    for (const auto &p : peers)
    {
        py::dict entry;
        entry["role_uid"] = p.role_uid;
        entry["pubkey"]   = p.pubkey;
        out.append(entry);
    }
    return out;
}

/// Mirror of Native engine's `fetch_band_members` (HEP-CORE-0030 broker
/// RPC `BAND_MEMBERS_REQ`/`_ACK`).  Issues the broker round-trip under
/// `py::gil_scoped_release`, then unwraps the `{"members": [...]}`
/// wrapper from `BAND_MEMBERS_ACK` and returns the bare array.
///
/// Single source of truth for the wire-unwrap step.  Collapses the
/// 6 buggy sites that hand-rolled the unwrap as `result->is_array()` —
/// see task #235 (the broker reply is an OBJECT wrapping the array, so
/// `is_array()` was always false and contains/count silently returned
/// false/0 across all 3 role APIs).
///
/// Throws `py::value_error` on transport failure (broker unreachable /
/// not-a-member / unknown channel) to match the existing contains/count
/// semantics.  Returns an empty array on success-but-no-members.
///
/// Native engine equivalent: `native_engine.cpp:572-583`.
inline nlohmann::json
fetch_band_members_or_throw(RoleAPIBase *base, const std::string &channel)
{
    std::optional<nlohmann::json> reply;
    {
        py::gil_scoped_release release;
        reply = base->band_members(channel);
    }
    if (!reply.has_value())
        throw py::value_error(
            "band_members transport failure for channel '" + channel + "'");
    // Broker reply shape (HEP-CORE-0030): { "members": [ {role_uid,
    // role_name}, ... ] }.  Unwrap the members array; default to empty
    // array if the field is absent (defensive — should always be
    // present per the BAND_MEMBERS_ACK schema).
    return reply->value("members", nlohmann::json::array());
}

} // namespace pylabhub::scripting::detail
