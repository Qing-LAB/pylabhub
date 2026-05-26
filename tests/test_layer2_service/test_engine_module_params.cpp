/**
 * @file test_engine_module_params.cpp
 * @brief L2 unit tests for `validate_fz_info_cache()`.
 *
 * Direct tests of the engine-startup-entry invariant checks.  Each
 * test crafts a FlexzoneInfoCache + (in_fz_spec, out_fz_spec) pair
 * that triggers exactly one of the four documented failure modes,
 * and asserts the resulting std::runtime_error message names the
 * specific violation.  Also a happy-path test for a correctly-
 * populated cache (returns normally; no throw).
 *
 * No engine, no role host, no SHM, no worker subprocess — these
 * tests verify the standalone validator function.
 */

#include "utils/engine_module_params.hpp"
#include "utils/role_api_base.hpp"
#include "utils/schema_def.hpp"
#include "utils/schema_utils.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace
{

using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::validate_fz_info_cache;
using pylabhub::hub::SchemaSpec;
using pylabhub::hub::FieldDef;

/// Helper: build a small SchemaSpec with one field (one f64 = 8 bytes
/// logical; align_to_physical_page(8) = PAGE_SIZE = typically 4096).
SchemaSpec make_simple_fz_spec()
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.packing    = "aligned";
    FieldDef fld;
    fld.name        = "value";
    fld.type_str    = "f64";
    fld.count       = 1;
    fld.length      = 0;
    spec.fields.push_back(fld);
    return spec;
}

/// Helper: build a correctly-populated cache that matches the given
/// per-side specs.
RoleAPIBase::FlexzoneInfoCache make_good_cache(const SchemaSpec &in_fz,
                                                const SchemaSpec &out_fz)
{
    RoleAPIBase::FlexzoneInfoCache c;
    c.has_tx_fz = out_fz.has_schema;
    if (c.has_tx_fz)
    {
        c.tx_logical_size  = pylabhub::hub::compute_schema_size(out_fz, out_fz.packing);
        c.tx_physical_size = pylabhub::hub::align_to_physical_page(c.tx_logical_size);
    }
    c.has_rx_fz = in_fz.has_schema;
    if (c.has_rx_fz)
    {
        c.rx_logical_size  = pylabhub::hub::compute_schema_size(in_fz, in_fz.packing);
        c.rx_physical_size = pylabhub::hub::align_to_physical_page(c.rx_logical_size);
    }
    return c;
}

} // namespace

// ============================================================================
// Happy path
// ============================================================================

TEST(EngineModuleParams_ValidateFzInfoCache, GoodCache_NoThrow)
{
    auto in_fz  = make_simple_fz_spec();
    auto out_fz = make_simple_fz_spec();
    auto cache  = make_good_cache(in_fz, out_fz);

    EXPECT_NO_THROW(validate_fz_info_cache(cache, in_fz, out_fz));
}

TEST(EngineModuleParams_ValidateFzInfoCache, NoFzAnywhere_NoThrow)
{
    // Empty specs (has_schema=false) + zeroed cache must validate.
    SchemaSpec empty_in;
    SchemaSpec empty_out;
    RoleAPIBase::FlexzoneInfoCache cache{};  // all zeros, both has_*_fz = false

    EXPECT_NO_THROW(validate_fz_info_cache(cache, empty_in, empty_out));
}

TEST(EngineModuleParams_ValidateFzInfoCache, TxOnly_GoodCache_NoThrow)
{
    // Producer-shaped: out_fz present, in_fz empty.
    auto out_fz = make_simple_fz_spec();
    SchemaSpec in_fz_empty;
    auto cache  = make_good_cache(in_fz_empty, out_fz);

    EXPECT_NO_THROW(validate_fz_info_cache(cache, in_fz_empty, out_fz));
}

// ============================================================================
// Negative path (a): cache ↔ params has_*_fz disagreement
// ============================================================================

TEST(EngineModuleParams_ValidateFzInfoCache, HasTxFzMismatch_Throws)
{
    auto out_fz = make_simple_fz_spec();  // params says TX has schema
    SchemaSpec in_fz_empty;

    RoleAPIBase::FlexzoneInfoCache bad_cache{};  // cache says has_tx_fz=false

    try
    {
        validate_fz_info_cache(bad_cache, in_fz_empty, out_fz);
        FAIL() << "expected std::runtime_error";
    }
    catch (const std::runtime_error &e)
    {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("has_tx_fz"), std::string::npos)
            << "error message must name the violated invariant: " << msg;
    }
}

TEST(EngineModuleParams_ValidateFzInfoCache, HasRxFzMismatch_Throws)
{
    SchemaSpec out_fz_empty;
    auto in_fz = make_simple_fz_spec();  // params says RX has schema

    RoleAPIBase::FlexzoneInfoCache bad_cache{};  // cache says has_rx_fz=false

    try
    {
        validate_fz_info_cache(bad_cache, in_fz, out_fz_empty);
        FAIL() << "expected std::runtime_error";
    }
    catch (const std::runtime_error &e)
    {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("has_rx_fz"), std::string::npos)
            << "error message must name the violated invariant: " << msg;
    }
}

TEST(EngineModuleParams_ValidateFzInfoCache, CacheClaimsTxButParamsDont_Throws)
{
    SchemaSpec out_fz_empty;
    SchemaSpec in_fz_empty;

    // Cache lies: claims has_tx_fz=true with bogus sizes.
    RoleAPIBase::FlexzoneInfoCache lying_cache{};
    lying_cache.has_tx_fz       = true;
    lying_cache.tx_logical_size = 8;
    lying_cache.tx_physical_size =
        pylabhub::hub::align_to_physical_page(lying_cache.tx_logical_size);

    EXPECT_THROW(validate_fz_info_cache(lying_cache, in_fz_empty, out_fz_empty),
                 std::runtime_error);
}

// ============================================================================
// Negative path (b): physical != align_to_physical_page(logical)
// ============================================================================

TEST(EngineModuleParams_ValidateFzInfoCache, TxPhysicalSizeNotPageAligned_Throws)
{
    auto out_fz = make_simple_fz_spec();
    SchemaSpec in_fz_empty;

    RoleAPIBase::FlexzoneInfoCache bad_cache{};
    bad_cache.has_tx_fz        = true;
    bad_cache.tx_logical_size  = pylabhub::hub::compute_schema_size(out_fz, out_fz.packing);
    // Deliberately wrong: should be align_to_physical_page(logical).
    bad_cache.tx_physical_size = bad_cache.tx_logical_size;  // un-aligned

    try
    {
        validate_fz_info_cache(bad_cache, in_fz_empty, out_fz);
        FAIL() << "expected std::runtime_error";
    }
    catch (const std::runtime_error &e)
    {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("tx_physical_size"), std::string::npos)
            << "error message must name tx_physical_size violation: " << msg;
        EXPECT_NE(msg.find("align_to_physical_page"), std::string::npos)
            << "error message must reference the alignment helper: " << msg;
    }
}

TEST(EngineModuleParams_ValidateFzInfoCache, RxPhysicalSizeNotPageAligned_Throws)
{
    auto in_fz = make_simple_fz_spec();
    SchemaSpec out_fz_empty;

    RoleAPIBase::FlexzoneInfoCache bad_cache{};
    bad_cache.has_rx_fz        = true;
    bad_cache.rx_logical_size  = pylabhub::hub::compute_schema_size(in_fz, in_fz.packing);
    // Deliberately wrong: physical = logical * 2 is not align_to_physical_page(logical).
    bad_cache.rx_physical_size = bad_cache.rx_logical_size * 2;

    try
    {
        validate_fz_info_cache(bad_cache, in_fz, out_fz_empty);
        FAIL() << "expected std::runtime_error";
    }
    catch (const std::runtime_error &e)
    {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("rx_physical_size"), std::string::npos)
            << "error message must name rx_physical_size violation: " << msg;
    }
}

// ============================================================================
// Mixed: cache fully consistent for the TX side but wrong for the RX side.
// Validator must catch the RX side error even when TX is correct.
// ============================================================================

TEST(EngineModuleParams_ValidateFzInfoCache, TxOk_RxBad_StillThrows)
{
    auto in_fz  = make_simple_fz_spec();
    auto out_fz = make_simple_fz_spec();

    auto cache = make_good_cache(in_fz, out_fz);
    // Corrupt only the RX side.
    cache.rx_physical_size = cache.rx_logical_size + 1;  // intentionally off-by-one

    EXPECT_THROW(validate_fz_info_cache(cache, in_fz, out_fz), std::runtime_error);
}
