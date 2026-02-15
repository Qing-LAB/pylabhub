/**
 * @file test_result.cpp
 * @brief Unit tests for Result<T, E> type
 * 
 * Tests the Result type used for error handling in the RAII layer.
 */

#include "utils/result.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace pylabhub::hub;

// Simple test error enum
enum class TestError
{
    NotFound,
    InvalidInput,
    Timeout
};

// ============================================================================
// Construction Tests
// ============================================================================

TEST(ResultTest, ConstructionOk)
{
    auto result = Result<int, TestError>::ok(42);
    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(result.is_error());
    EXPECT_EQ(result.content(), 42);
}

TEST(ResultTest, ConstructionError)
{
    auto result = Result<int, TestError>::error(TestError::NotFound, 123);
    EXPECT_FALSE(result.is_ok());
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error(), TestError::NotFound);
    EXPECT_EQ(result.error_code(), 123);
}

TEST(ResultTest, ConstructionErrorDefaultCode)
{
    auto result = Result<int, TestError>::error(TestError::Timeout);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error(), TestError::Timeout);
    EXPECT_EQ(result.error_code(), 0);
}

// ============================================================================
// Value Access Tests
// ============================================================================

TEST(ResultTest, ValueAccessSuccess)
{
    auto result = Result<std::string, TestError>::ok("hello");
    EXPECT_EQ(result.value(), "hello");

    // Test mutable access
    result.content() = "world";
    EXPECT_EQ(result.content(), "world");
}

TEST(ResultTest, ValueAccessThrowsOnError)
{
    auto result = Result<int, TestError>::error(TestError::NotFound);
    EXPECT_THROW({ (void)result.content(); }, std::logic_error);
}

TEST(ResultTest, ValueOrReturnsValueWhenOk)
{
    auto result = Result<int, TestError>::ok(42);
    EXPECT_EQ(result.value_or(99), 42);
}

TEST(ResultTest, ValueOrReturnsDefaultWhenError)
{
    auto result = Result<int, TestError>::error(TestError::NotFound);
    EXPECT_EQ(result.value_or(99), 99);
}

// ============================================================================
// Error Access Tests
// ============================================================================

TEST(ResultTest, ErrorAccessSuccess)
{
    auto result = Result<int, TestError>::error(TestError::InvalidInput, 456);
    EXPECT_EQ(result.error(), TestError::InvalidInput);
    EXPECT_EQ(result.error_code(), 456);
}

TEST(ResultTest, ErrorAccessThrowsOnSuccess)
{
    auto result = Result<int, TestError>::ok(42);
    EXPECT_THROW({ (void)result.error(); }, std::logic_error);
    EXPECT_THROW({ (void)result.error_code(); }, std::logic_error);
}

// ============================================================================
// Move Semantics Tests
// ============================================================================

TEST(ResultTest, MoveConstruction)
{
    auto result1 = Result<std::string, TestError>::ok("moved");
    auto result2 = std::move(result1);

    EXPECT_TRUE(result2.is_ok());
    EXPECT_EQ(result2.value(), "moved");
}

TEST(ResultTest, MoveAssignment)
{
    auto result1 = Result<std::string, TestError>::ok("moved");
    auto result2 = Result<std::string, TestError>::error(TestError::NotFound);

    result2 = std::move(result1);

    EXPECT_TRUE(result2.is_ok());
    EXPECT_EQ(result2.value(), "moved");
}

TEST(ResultTest, MoveValue)
{
    auto result = Result<std::string, TestError>::ok("hello");
    std::string value = std::move(result).content();

    EXPECT_EQ(value, "hello");
}

// ============================================================================
// SlotAcquireError Tests
// ============================================================================

TEST(SlotAcquireErrorTest, ToString)
{
    EXPECT_STREQ(to_string(SlotAcquireError::Timeout), "Timeout");
    EXPECT_STREQ(to_string(SlotAcquireError::NoSlot), "NoSlot");
    EXPECT_STREQ(to_string(SlotAcquireError::Error), "Error");
}

TEST(SlotAcquireErrorTest, ResultWithSlotAcquireError)
{
    auto timeout_result = Result<int, SlotAcquireError>::error(SlotAcquireError::Timeout);
    EXPECT_TRUE(timeout_result.is_error());
    EXPECT_EQ(timeout_result.error(), SlotAcquireError::Timeout);

    auto no_slot_result = Result<int, SlotAcquireError>::error(SlotAcquireError::NoSlot);
    EXPECT_TRUE(no_slot_result.is_error());
    EXPECT_EQ(no_slot_result.error(), SlotAcquireError::NoSlot);

    auto error_result = Result<int, SlotAcquireError>::error(SlotAcquireError::Error, 999);
    EXPECT_TRUE(error_result.is_error());
    EXPECT_EQ(error_result.error(), SlotAcquireError::Error);
    EXPECT_EQ(error_result.error_code(), 999);
}

// ============================================================================
// Type Traits Tests
// ============================================================================

TEST(ResultTest, TypeTraits)
{
    // Verify Result is movable
    static_assert(std::is_move_constructible_v<Result<int, TestError>>);
    static_assert(std::is_move_assignable_v<Result<int, TestError>>);

    // Verify Result is not copyable (by design)
    static_assert(!std::is_copy_constructible_v<Result<int, TestError>>);
    static_assert(!std::is_copy_assignable_v<Result<int, TestError>>);
}
