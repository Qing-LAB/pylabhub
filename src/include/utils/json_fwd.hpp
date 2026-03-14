#pragma once
// utils/json_fwd.hpp — Project-level JSON abstraction layer
//
// All source files include this header instead of <nlohmann/json.hpp>
// directly.  If the project ever switches JSON libraries, only this
// file (and the .cpp files that use library-specific API) need to change.

#include <nlohmann/json.hpp>
