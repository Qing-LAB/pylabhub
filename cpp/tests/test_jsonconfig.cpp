// test_jsonconfig.cpp
// Tests JsonConfig basic behavior: init(createIfMissing), with_json_write exclusivity, save()
// refusal while exclusive, then save success.

#include <cassert>
#include <filesystem>
#include <iostream>

#include "fileutil/JsonConfig.hpp"

using namespace pylabhub::fileutil;

int main()
{
    namespace fs = std::filesystem;
    fs::path tmpdir = fs::temp_directory_path();
    fs::path cfgpath = tmpdir / "pylabhub_test_jsonconfig.json";

    // Ensure removed to start fresh
    try
    {
        fs::remove(cfgpath);
    }
    catch (...)
    {
    }

    JsonConfig cfg;
    if (!cfg.init(cfgpath, /*createIfMissing*/ true))
    {
        std::cerr << "test_jsonconfig: init(createIfMissing) failed\n";
        return 2;
    }

    // Replace content with known JSON
    nlohmann::json j;
    j["value"] = 123;
    if (!cfg.replace(j))
    {
        std::cerr << "test_jsonconfig: replace() failed\n";
        return 3;
    }

    // Start an exclusive with_json_write and inside callback attempt to call save() (should fail)
    bool cb_ok = cfg.with_json_write(
        [&](nlohmann::json &m)
        {
            // modify in-memory
            m["value"] = 456;
            // Attempt to save while exclusive guard is active: per policy this should fail
            bool saved = cfg.save();
            if (saved)
            {
                std::cerr << "test_jsonconfig: save() unexpectedly succeeded from inside "
                             "with_json_write\n";
            }
            assert(!saved);
        });
    if (!cb_ok)
    {
        std::cerr << "test_jsonconfig: with_json_write callback failed\n";
        return 4;
    }

    // Now save() should succeed now that exclusive guard released
    if (!cfg.save())
    {
        std::cerr << "test_jsonconfig: save() failed after exclusive guard released\n";
        return 5;
    }

    // reload into a fresh instance and check the value persisted
    JsonConfig cfg2;
    if (!cfg2.init(cfgpath, /*createIfMissing*/ false))
    {
        std::cerr << "test_jsonconfig: cfg2.init failed\n";
        return 6;
    }
    auto val = cfg2.get_optional<int>("value");
    if (!val || *val != 456)
    {
        std::cerr << "test_jsonconfig: persisted value mismatch (expected 456)\n";
        return 7;
    }

    // cleanup
    try
    {
        fs::remove(cfgpath);
    }
    catch (...)
    {
    }

    std::cout << "test_jsonconfig: OK\n";
    return 0;
}
