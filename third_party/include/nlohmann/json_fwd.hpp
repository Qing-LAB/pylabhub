// nlohmann/json_fwd.hpp — Forward declarations for nlohmann::json
//
// This header provides minimal forward declarations so that API headers
// (producer_api.hpp, consumer_api.hpp, processor_api.hpp) can reference
// nlohmann::json without pulling in the full 20K-line json.hpp.
//
// It also serves as an abstraction layer: if the project ever switches
// JSON libraries, only this header and the .cpp files need to change —
// downstream API headers remain untouched.
//
// Based on nlohmann/json v3.11 forward declarations.

#ifndef NLOHMANN_JSON_FWD_HPP
#define NLOHMANN_JSON_FWD_HPP

#include <cstdint>   // int64_t, uint64_t
#include <map>       // map (default ObjectType)
#include <memory>    // allocator
#include <string>    // string (default StringType)
#include <vector>    // vector (default ArrayType)

namespace nlohmann
{

template <typename T, typename SFINAE>
struct adl_serializer;

template <template <typename U, typename V, typename... Args> class ObjectType,
          class ArrayType,
          class StringType,
          class BooleanType,
          class NumberIntegerType,
          class NumberUnsignedType,
          class NumberFloatType,
          template <typename U> class AllocatorType,
          template <typename T, typename SFINAE> class JSONSerializer,
          class BinaryType,
          class CustomBaseClass>
class basic_json;

using json = basic_json<std::map,
                        std::vector,
                        std::string,
                        bool,
                        std::int64_t,
                        std::uint64_t,
                        double,
                        std::allocator,
                        adl_serializer,
                        std::vector<std::uint8_t>,
                        void>;

} // namespace nlohmann

#endif // NLOHMANN_JSON_FWD_HPP
