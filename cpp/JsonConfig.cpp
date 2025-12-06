#include "utils/JsonConfig.hpp"

#include <fstream>

namespace pylabhub::utils {

JsonConfig::JsonConfig(const std::string& filepath) {
    std::ifstream file(filepath);
    if (file.is_open()) {
        // Allow exceptions to be thrown on parsing errors
        data = nlohmann::json::parse(file, nullptr, true, true);
    }
    // If the file can't be opened, 'data' remains in a default null state.
}

} // namespace pylabhub::utils