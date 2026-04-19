/**
 * @file test_scriptengine_native_dylib.cpp
 * @brief Pattern 3 driver: NativeEngine test suite.
 *
 * Original V2: every body fabricated a RoleAPIBase in the gtest runner
 * without a LifecycleGuard — that's the failure mode HEP-CORE-0001 §
 * "Testing implications" warns against (ThreadManager constructor calls
 * register_dynamic_module against an uninitialised LifecycleManager and
 * leaves half-state).
 *
 * Each TEST_F now spawns a worker (workers/scriptengine_native_dylib_workers.cpp).
 * The parent passes TEST_PLUGIN_DIR (the build-time plugin .so stage
 * directory) as argv[2] so the subprocess can locate the same .so
 * artefacts the build produced.
 */
#include "test_patterns.h"

#include <gtest/gtest.h>

#include <string>

#ifndef TEST_PLUGIN_DIR
#   define TEST_PLUGIN_DIR "."
#endif

using pylabhub::tests::IsolatedProcessTest;

namespace
{

class NativeEngineTest : public IsolatedProcessTest
{
  protected:
    /// Plugin search directory for the subprocess.
    static std::string plugin_dir() { return TEST_PLUGIN_DIR; }
};

} // namespace

TEST_F(NativeEngineTest, FullLifecycle_ProduceCommitsAndWritesSlot)
{
    auto w = SpawnWorker(
        "native_engine.full_lifecycle_produce_commits_and_writes_slot",
        {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, HasCallback_ReflectsPluginSymbols)
{
    auto w = SpawnWorker("native_engine.has_callback_reflects_plugin_symbols",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, SchemaValidation_MatchingSchema_Succeeds)
{
    auto w = SpawnWorker(
        "native_engine.schema_validation_matching_schema_succeeds",
        {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, SchemaValidation_HasSchemaFalse_ReturnsFalse)
{
    auto w = SpawnWorker(
        "native_engine.schema_validation_has_schema_false_returns_false",
        {plugin_dir()});
    // NativeEngine logs ERROR when register_slot_type is called with a
    // schema whose has_schema flag is false (the engine requires a schema
    // to register a slot type).
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"schema"});
}

TEST_F(NativeEngineTest, SchemaValidation_MismatchedSchema_Fails)
{
    auto w = SpawnWorker(
        "native_engine.schema_validation_mismatched_schema_fails",
        {plugin_dir()});
    // register_slot_type logs the mismatch between the declared schema
    // and the one the plugin exported.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"schema"});
}

TEST_F(NativeEngineTest, LoadScript_MissingFile_ReturnsFalse)
{
    auto w = SpawnWorker("native_engine.load_script_missing_file_returns_false",
                         {});
    // NativeEngine logs "native engine not found: <path>" before returning false.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"native engine not found"});
}

TEST_F(NativeEngineTest, LoadScript_MissingRequiredCallback_ReturnsFalse)
{
    auto w = SpawnWorker(
        "native_engine.load_script_missing_required_callback_returns_false",
        {plugin_dir()});
    // Logs that the required callback symbol is missing from the plugin.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"callback"});
}

TEST_F(NativeEngineTest, Eval_ReturnsNotFound)
{
    auto w = SpawnWorker("native_engine.eval_returns_not_found", {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, GenericInvoke_KnownCallback_ReturnsTrue)
{
    auto w = SpawnWorker("native_engine.generic_invoke_known_callback_returns_true",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, SupportsMultiState_DefaultFalse)
{
    auto w = SpawnWorker("native_engine.supports_multi_state_default_false",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, ContextFieldsPassedToPlugin)
{
    auto w = SpawnWorker("native_engine.context_fields_passed_to_plugin",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, Checksum_WrongHash_RejectsPlugin)
{
    auto w = SpawnWorker("native_engine.checksum_wrong_hash_rejects_plugin",
                         {plugin_dir()});
    // Wrong SHA-256 → load_script logs "native checksum mismatch" + refuses to load.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"checksum"});
}

TEST_F(NativeEngineTest, Checksum_EmptyHash_SkipsVerification)
{
    auto w = SpawnWorker("native_engine.checksum_empty_hash_skips_verification",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, Api_CountersAndSchemaSize_ThroughNativeModule)
{
    auto w = SpawnWorker(
        "native_engine.api_counters_and_schema_size_through_native_module",
        {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, FullStartup_Producer_SlotOnly)
{
    auto w = SpawnWorker("native_engine.full_startup_producer_slot_only",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, FullStartup_Consumer)
{
    auto w = SpawnWorker("native_engine.full_startup_consumer", {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, FullStartup_Processor)
{
    auto w = SpawnWorker("native_engine.full_startup_processor", {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, FullStartup_Producer_Multifield)
{
    auto w = SpawnWorker("native_engine.full_startup_producer_multifield",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, FullStartup_Consumer_Multifield)
{
    auto w = SpawnWorker("native_engine.full_startup_consumer_multifield",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, FullStartup_Processor_Multifield)
{
    auto w = SpawnWorker("native_engine.full_startup_processor_multifield",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, FullStartup_Producer_SlotAndFlexzone)
{
    auto w = SpawnWorker(
        "native_engine.full_startup_producer_slot_and_flexzone",
        {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, InvokeOnInbox_TypedData)
{
    auto w = SpawnWorker("native_engine.invoke_on_inbox_typed_data",
                         {plugin_dir()});
    ExpectWorkerOk(w);
}

TEST_F(NativeEngineTest, Api_BandPubSub_NoBroker_GracefulReturn)
{
    auto w = SpawnWorker(
        "native_engine.api_band_pub_sub_no_broker_graceful_return",
        {plugin_dir()});
    ExpectWorkerOk(w);
}
