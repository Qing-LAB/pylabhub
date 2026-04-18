/**
 * @file test_role_registry.cpp
 * @brief L2 tests for RoleRegistry + RuntimeBuilder.
 *
 * The registry is a process-global singleton — each test uses a unique
 * role_tag (suffixed with an atomic counter) so runs don't collide.
 */
#include "utils/role_registry.hpp"
#include "utils/role_host_base.hpp"

#include <gtest/gtest.h>

#include <any>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

using pylabhub::utils::RoleRegistry;
using pylabhub::utils::RoleRuntimeInfo;

namespace
{

// Dummy factory used for valid-registration tests. Never actually invoked
// (the registry only stores the pointer); we return nullptr to keep the
// implementation trivial.
std::unique_ptr<pylabhub::scripting::RoleHostBase>
dummy_host_factory(pylabhub::config::RoleConfig,
                    std::unique_ptr<pylabhub::scripting::ScriptEngine>,
                    std::atomic<bool> *)
{
    return nullptr;
}

std::any dummy_config_parser(const nlohmann::json &,
                              const pylabhub::config::RoleConfig &)
{
    return std::any{};
}

// Generate a unique tag per call — registrations accumulate across tests
// in the same process, so collisions must be avoided by construction.
std::string unique_tag(const char *prefix)
{
    static std::atomic<int> counter{0};
    return std::string(prefix) + "_" + std::to_string(counter.fetch_add(1));
}

} // namespace

// ─── Basic register + lookup ────────────────────────────────────────────

TEST(RoleRegistryTest, RegisterAndGet_PopulatedInfo)
{
    const auto tag = unique_tag("rr_basic");
    RoleRegistry::register_runtime(tag)
        .role_label("RR Basic")
        .host_factory(&dummy_host_factory)
        .config_parser(&dummy_config_parser)
        .commit();

    const RoleRuntimeInfo *info = RoleRegistry::get_runtime(tag);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->role_tag,      tag);
    EXPECT_EQ(info->role_label,    "RR Basic");
    EXPECT_EQ(info->host_factory,  &dummy_host_factory);
    EXPECT_EQ(info->config_parser, &dummy_config_parser);
}

TEST(RoleRegistryTest, Get_UnknownTag_Nullptr)
{
    EXPECT_EQ(RoleRegistry::get_runtime("never_registered_xyz_12345"), nullptr);
}

TEST(RoleRegistryTest, Get_AcceptsStringView_NoAllocationSemantics)
{
    const auto tag = unique_tag("rr_sv");
    RoleRegistry::register_runtime(tag)
        .role_label("SV").host_factory(&dummy_host_factory).commit();

    std::string_view sv(tag);
    EXPECT_NE(RoleRegistry::get_runtime(sv), nullptr);
}

// ─── Duplicate registration ──────────────────────────────────────────────

TEST(RoleRegistryTest, DuplicateRegister_CommitThrows)
{
    const auto tag = unique_tag("rr_dup");
    RoleRegistry::register_runtime(tag)
        .role_label("First").host_factory(&dummy_host_factory).commit();

    auto second = RoleRegistry::register_runtime(tag);
    second.role_label("Second").host_factory(&dummy_host_factory);
    EXPECT_THROW(second.commit(), std::runtime_error);

    // Original entry unchanged.
    const auto *info = RoleRegistry::get_runtime(tag);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->role_label, "First");
}

// ─── Missing host_factory ────────────────────────────────────────────────

TEST(RoleRegistryTest, CommitWithoutHostFactory_Throws)
{
    const auto tag = unique_tag("rr_nofactory");
    auto b = RoleRegistry::register_runtime(tag);
    b.role_label("No Factory");
    // host_factory deliberately not set.
    EXPECT_THROW(b.commit(), std::runtime_error);

    // Not registered.
    EXPECT_EQ(RoleRegistry::get_runtime(tag), nullptr);
}

TEST(RoleRegistryTest, CommitWithoutHostFactory_ErrorMentionsRoleTag)
{
    const auto tag = unique_tag("rr_nofactory_msg");
    auto b = RoleRegistry::register_runtime(tag);
    b.role_label("No Factory");
    try
    {
        b.commit();
        FAIL() << "expected exception";
    }
    catch (const std::runtime_error &e)
    {
        const std::string msg(e.what());
        EXPECT_NE(msg.find(tag),           std::string::npos);
        EXPECT_NE(msg.find("host_factory"), std::string::npos);
    }
}

// ─── Multiple different roles coexist ────────────────────────────────────

TEST(RoleRegistryTest, MultipleRoles_EachIndependentlyLookedUp)
{
    const auto a = unique_tag("rr_multi_a");
    const auto b = unique_tag("rr_multi_b");
    const auto c = unique_tag("rr_multi_c");
    RoleRegistry::register_runtime(a)
        .role_label("A").host_factory(&dummy_host_factory).commit();
    RoleRegistry::register_runtime(b)
        .role_label("B").host_factory(&dummy_host_factory).commit();
    RoleRegistry::register_runtime(c)
        .role_label("C").host_factory(&dummy_host_factory).commit();

    EXPECT_EQ(RoleRegistry::get_runtime(a)->role_label, "A");
    EXPECT_EQ(RoleRegistry::get_runtime(b)->role_label, "B");
    EXPECT_EQ(RoleRegistry::get_runtime(c)->role_label, "C");
}

// ─── Builder commit idempotency + auto-commit ────────────────────────────

TEST(RoleRegistryTest, Commit_Idempotent)
{
    const auto tag = unique_tag("rr_idem");
    auto b = RoleRegistry::register_runtime(tag);
    b.role_label("Idem").host_factory(&dummy_host_factory);
    b.commit();
    EXPECT_NO_THROW(b.commit());  // second commit is a no-op

    const auto *info = RoleRegistry::get_runtime(tag);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->role_label, "Idem");
}

TEST(RoleRegistryTest, BuilderDtor_AutoCommits_WhenFactorySet)
{
    const auto tag = unique_tag("rr_auto");
    {
        auto b = RoleRegistry::register_runtime(tag);
        b.role_label("Auto").host_factory(&dummy_host_factory);
        // No explicit .commit() — dtor must finish the registration.
    }
    const auto *info = RoleRegistry::get_runtime(tag);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->role_label, "Auto");
}

TEST(RoleRegistryTest, BuilderDtor_MissingFactory_SwallowsException)
{
    const auto tag = unique_tag("rr_auto_nofactory");
    {
        auto b = RoleRegistry::register_runtime(tag);
        b.role_label("Missing");
        // Dtor's commit() throws internally (no factory); must be swallowed
        // to satisfy noexcept(dtor). Entry is not inserted.
    }
    EXPECT_EQ(RoleRegistry::get_runtime(tag), nullptr);
}

// ─── Optional config_parser (may be nullptr) ─────────────────────────────

TEST(RoleRegistryTest, ConfigParser_MayBeNullptr)
{
    const auto tag = unique_tag("rr_noparser");
    RoleRegistry::register_runtime(tag)
        .role_label("No Parser")
        .host_factory(&dummy_host_factory)
        // no .config_parser() call
        .commit();
    const auto *info = RoleRegistry::get_runtime(tag);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->config_parser, nullptr);
}
