/**
 * @file test_role_config.cpp
 * @brief Pattern 3 driver: RoleConfig unit-test suite.
 *
 * Original V1: SetUpTestSuite installed a process-wide LifecycleGuard so
 * each TEST_F could call RoleConfig::load directly. Per HEP-CORE-0001
 * § "Testing implications" the process-wide guard is forbidden — every
 * lifecycle-dependent test runs in its own subprocess.
 *
 * Each TEST_F here spawns one worker (workers/role_config_workers.cpp).
 * The worker writes a minimal JSON config inside a parent-provided unique
 * scratch dir, calls RoleConfig::load*, and asserts on the parsed fields.
 * The parent removes the dir after wait_for_exit.
 *
 * No body-wrapping macros — each TEST_F is written out plainly so gtest
 * failure messages report the correct line and the structure stays
 * grep-able.
 */
#include "test_patterns.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;

namespace
{

class RoleConfigTest : public IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    /// Returns a unique scratch dir; auto-cleaned after the test.
    std::string unique_dir(const char *test_name)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_rcfg_" + std::string(test_name) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

// ── Producer ────────────────────────────────────────────────────────────────

TEST_F(RoleConfigTest, LoadProducer_Identity)
{
    auto w = SpawnWorker("role_config.load_producer_identity",
                         {unique_dir("load_producer_identity")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadProducer_Timing)
{
    auto w = SpawnWorker("role_config.load_producer_timing",
                         {unique_dir("load_producer_timing")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadProducer_Script)
{
    auto w = SpawnWorker("role_config.load_producer_script",
                         {unique_dir("load_producer_script")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadProducer_OutChannel)
{
    auto w = SpawnWorker("role_config.load_producer_out_channel",
                         {unique_dir("load_producer_out_channel")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadProducer_OutTransport)
{
    auto w = SpawnWorker("role_config.load_producer_out_transport",
                         {unique_dir("load_producer_out_transport")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadProducer_OutValidation)
{
    auto w = SpawnWorker("role_config.load_producer_out_validation",
                         {unique_dir("load_producer_out_validation")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadProducer_Validation_StopOnScriptError)
{
    auto w = SpawnWorker("role_config.load_producer_validation_stop_on_script_error",
                         {unique_dir("stop_on_script_error")});
    ExpectWorkerOk(w);
}

// ── Consumer ────────────────────────────────────────────────────────────────

TEST_F(RoleConfigTest, LoadConsumer_Identity)
{
    auto w = SpawnWorker("role_config.load_consumer_identity",
                         {unique_dir("load_consumer_identity")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadConsumer_InChannel)
{
    auto w = SpawnWorker("role_config.load_consumer_in_channel",
                         {unique_dir("load_consumer_in_channel")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadConsumer_InValidation)
{
    auto w = SpawnWorker("role_config.load_consumer_in_validation",
                         {unique_dir("load_consumer_in_validation")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadConsumer_DefaultTiming)
{
    auto w = SpawnWorker("role_config.load_consumer_default_timing",
                         {unique_dir("load_consumer_default_timing")});
    ExpectWorkerOk(w);
}

// ── Processor ───────────────────────────────────────────────────────────────

TEST_F(RoleConfigTest, LoadProcessor_DualChannels)
{
    auto w = SpawnWorker("role_config.load_processor_dual_channels",
                         {unique_dir("load_processor_dual_channels")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadProcessor_DualTransport)
{
    auto w = SpawnWorker("role_config.load_processor_dual_transport",
                         {unique_dir("load_processor_dual_transport")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadProcessor_DualValidation)
{
    auto w = SpawnWorker("role_config.load_processor_dual_validation",
                         {unique_dir("load_processor_dual_validation")});
    ExpectWorkerOk(w);
}

// ── Role-specific data ──────────────────────────────────────────────────────

TEST_F(RoleConfigTest, RoleData_ProducerFields)
{
    auto w = SpawnWorker("role_config.role_data_producer_fields",
                         {unique_dir("role_data_producer_fields")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, RoleData_NoParser)
{
    auto w = SpawnWorker("role_config.role_data_no_parser",
                         {unique_dir("role_data_no_parser")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, RoleData_WrongTypeCast_Throws)
{
    auto w = SpawnWorker("role_config.role_data_wrong_type_cast_throws",
                         {unique_dir("role_data_wrong_type_cast_throws")});
    ExpectWorkerOk(w);
}

// ── Auth ────────────────────────────────────────────────────────────────────

TEST_F(RoleConfigTest, Auth_DefaultEmpty)
{
    auto w = SpawnWorker("role_config.auth_default_empty",
                         {unique_dir("auth_default_empty")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadKeypair_NoKeyfile_ReturnsFalse)
{
    auto w = SpawnWorker("role_config.load_keypair_no_keyfile_returns_false",
                         {unique_dir("load_keypair_no_keyfile")});
    ExpectWorkerOk(w);
}

// ── Raw JSON / metadata ─────────────────────────────────────────────────────

TEST_F(RoleConfigTest, RawJson)
{
    auto w = SpawnWorker("role_config.raw_json", {unique_dir("raw_json")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, RoleTag)
{
    auto w = SpawnWorker("role_config.role_tag", {unique_dir("role_tag")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, BaseDir)
{
    auto w = SpawnWorker("role_config.base_dir", {unique_dir("base_dir")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoadFromDirectory)
{
    auto w = SpawnWorker("role_config.load_from_directory",
                         {unique_dir("load_from_directory")});
    ExpectWorkerOk(w);
}

// ── Validation errors ───────────────────────────────────────────────────────

// Validation-error tests (file_not_found, invalid_script_type,
// zmq_missing_endpoint, checksum_invalid, unknown_key, logging_*):
// The workers catch the throw with a try/catch and assert on the
// exception's what() text — that verifies the contract of what message
// the user sees, without relying on log-line substring matching. The
// parent here only needs to confirm the worker exited cleanly.
TEST_F(RoleConfigTest, FileNotFound_Throws)
{
    auto w = SpawnWorker("role_config.file_not_found_throws", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, InvalidScriptType_Throws)
{
    auto w = SpawnWorker("role_config.invalid_script_type_throws",
                         {unique_dir("invalid_script_type_throws")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, ZmqTransport_MissingEndpoint_Throws)
{
    auto w = SpawnWorker("role_config.zmq_transport_missing_endpoint_throws",
                         {unique_dir("zmq_missing_endpoint")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, ZmqTransport_Valid)
{
    auto w = SpawnWorker("role_config.zmq_transport_valid",
                         {unique_dir("zmq_transport_valid")});
    ExpectWorkerOk(w);
}

// ── Move semantics ──────────────────────────────────────────────────────────

TEST_F(RoleConfigTest, MoveConstruct)
{
    auto w = SpawnWorker("role_config.move_construct",
                         {unique_dir("move_construct")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, MoveAssign)
{
    auto w = SpawnWorker("role_config.move_assign", {unique_dir("move_assign")});
    ExpectWorkerOk(w);
}

// ── Checksum ────────────────────────────────────────────────────────────────

TEST_F(RoleConfigTest, ChecksumDefault_Enforced)
{
    auto w = SpawnWorker("role_config.checksum_default_enforced",
                         {unique_dir("checksum_default_enforced")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, ChecksumExplicit_Manual)
{
    auto w = SpawnWorker("role_config.checksum_explicit_manual",
                         {unique_dir("checksum_explicit_manual")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, ChecksumExplicit_None)
{
    auto w = SpawnWorker("role_config.checksum_explicit_none",
                         {unique_dir("checksum_explicit_none")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, ChecksumInvalid_Throws)
{
    auto w = SpawnWorker("role_config.checksum_invalid_throws",
                         {unique_dir("checksum_invalid_throws")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, ChecksumNull_DefaultEnforced)
{
    auto w = SpawnWorker("role_config.checksum_null_default_enforced",
                         {unique_dir("checksum_null_default_enforced")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, UnknownKey_Throws)
{
    auto w = SpawnWorker("role_config.unknown_key_throws",
                         {unique_dir("unknown_key_throws")});
    ExpectWorkerOk(w);
}

// ── Logging ─────────────────────────────────────────────────────────────────

TEST_F(RoleConfigTest, LoggingDefault_AllDefaults)
{
    auto w = SpawnWorker("role_config.logging_default_all_defaults",
                         {unique_dir("logging_default_all_defaults")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoggingExplicit_AllFields)
{
    auto w = SpawnWorker("role_config.logging_explicit_all_fields",
                         {unique_dir("logging_explicit_all_fields")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoggingPartial_MixesDefaultsAndExplicit)
{
    auto w = SpawnWorker(
        "role_config.logging_partial_mixes_defaults_and_explicit",
        {unique_dir("logging_partial_mixes")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoggingZeroBackups_Throws)
{
    auto w = SpawnWorker("role_config.logging_zero_backups_throws",
                         {unique_dir("logging_zero_backups_throws")});
    ExpectWorkerOk(w);  // see NOTE above re: missing LOGGER_ERROR_RT
}

TEST_F(RoleConfigTest, LoggingBackupsNegativeOne_KeepsAllFiles)
{
    auto w = SpawnWorker("role_config.logging_backups_negative_one_keeps_all_files",
                         {unique_dir("logging_backups_negative_one")});
    ExpectWorkerOk(w);
}

TEST_F(RoleConfigTest, LoggingNegativeBackupsOther_Throws)
{
    auto w = SpawnWorker("role_config.logging_negative_backups_other_throws",
                         {unique_dir("logging_negative_backups_other")});
    ExpectWorkerOk(w);  // see NOTE above re: missing LOGGER_ERROR_RT
}

TEST_F(RoleConfigTest, LoggingZeroMaxSize_Throws)
{
    auto w = SpawnWorker("role_config.logging_zero_max_size_throws",
                         {unique_dir("logging_zero_max_size_throws")});
    ExpectWorkerOk(w);  // see NOTE above re: missing LOGGER_ERROR_RT
}

TEST_F(RoleConfigTest, LoggingNegativeMaxSize_Throws)
{
    auto w = SpawnWorker("role_config.logging_negative_max_size_throws",
                         {unique_dir("logging_negative_max_size_throws")});
    ExpectWorkerOk(w);  // see NOTE above re: missing LOGGER_ERROR_RT
}

TEST_F(RoleConfigTest, LoggingUnknownSubKey_Throws)
{
    auto w = SpawnWorker("role_config.logging_unknown_sub_key_throws",
                         {unique_dir("logging_unknown_sub_key_throws")});
    ExpectWorkerOk(w);  // see NOTE above re: missing LOGGER_ERROR_RT
}

TEST_F(RoleConfigTest, LoggingNotObject_Throws)
{
    auto w = SpawnWorker("role_config.logging_not_object_throws",
                         {unique_dir("logging_not_object_throws")});
    ExpectWorkerOk(w);  // see NOTE above re: missing LOGGER_ERROR_RT
}

TEST_F(RoleConfigTest, LoggingFractionalMaxSize_Accepted)
{
    auto w = SpawnWorker("role_config.logging_fractional_max_size_accepted",
                         {unique_dir("logging_fractional_max_size")});
    ExpectWorkerOk(w);
}
