#pragma once
/**
 * @file test_datahub_types.h
 * @brief Shared test types and schemas for DataHub tests
 * 
 * Provides common test data structures with BLDS schema definitions.
 * Use these instead of defining ad-hoc structs in individual tests.
 */

#include "plh_datahub.hpp"
#include <cstdint>
#include <cstring>

namespace pylabhub::tests {

// ============================================================================
// Empty FlexZone (for tests that don't need flex zone)
// ============================================================================

/**
 * @brief Empty flex zone struct (for tests without flex zone data)
 * Use instead of std::monostate (which has no schema)
 */
struct EmptyFlexZone {
    // Empty - no fields
};

// Legacy alias
using NoFlexZone = EmptyFlexZone;

// ============================================================================
// Simple Test Types
// ============================================================================

/**
 * @brief Simple flex zone with counter and timestamp
 * Use for basic flex zone tests
 */
struct TestFlexZone {
    uint64_t counter;
    uint64_t timestamp_ns;
    
    TestFlexZone() : counter(0), timestamp_ns(0) {}
    TestFlexZone(uint64_t c, uint64_t t) : counter(c), timestamp_ns(t) {}
};

/**
 * @brief Simple data block with sequence, value, and label
 * Use for basic slot tests
 */
struct TestDataBlock {
    uint64_t sequence;
    uint64_t value;
    char label[16];
    
    TestDataBlock() : sequence(0), value(0), label{} {}
    TestDataBlock(uint64_t seq, uint64_t val, const char* lbl) 
        : sequence(seq), value(val), label{} 
    {
        if (lbl) {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996) // strncpy: safe — bounded by sizeof(label)-1, null-terminated below
#endif
            std::strncpy(label, lbl, sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        }
    }
};

/**
 * @brief Minimal data block (single field)
 * Use for stress tests where data size matters
 */
struct MinimalData {
    uint64_t id;
    
    MinimalData() : id(0) {}
    explicit MinimalData(uint64_t i) : id(i) {}
};

/**
 * @brief Large data block for testing large slot sizes
 * Use for testing physical page alignment, large data throughput
 */
struct LargeTestData {
    uint64_t id;
    uint64_t timestamp_ns;
    char payload[1008]; // Total 1024 bytes with padding
    
    LargeTestData() : id(0), timestamp_ns(0), payload{} {}
    LargeTestData(uint64_t i, uint64_t t) : id(i), timestamp_ns(t), payload{} {}
};

// ============================================================================
// Structured Types for Specific Test Scenarios
// ============================================================================

/**
 * @brief Frame metadata (for video/camera tests)
 */
struct FrameMeta {
    uint64_t frame_number;
    uint64_t timestamp_ns;
    
    FrameMeta() : frame_number(0), timestamp_ns(0) {}
    FrameMeta(uint64_t fn, uint64_t ts) : frame_number(fn), timestamp_ns(ts) {}
};

/**
 * @brief Sensor data (for IoT/sensor tests)
 */
struct SensorData {
    uint64_t timestamp_ns;
    double temperature;
    double humidity;
    uint32_t sensor_id;
    
    SensorData() : timestamp_ns(0), temperature(0.0), humidity(0.0), sensor_id(0) {}
};

} // namespace pylabhub::tests

// ============================================================================
// BLDS Schema Definitions
// ============================================================================

PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::EmptyFlexZone)
PYLABHUB_SCHEMA_END(pylabhub::tests::EmptyFlexZone)

PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::TestFlexZone)
    PYLABHUB_SCHEMA_MEMBER(counter)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
PYLABHUB_SCHEMA_END(pylabhub::tests::TestFlexZone)

PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::TestDataBlock)
    PYLABHUB_SCHEMA_MEMBER(sequence)
    PYLABHUB_SCHEMA_MEMBER(value)
    PYLABHUB_SCHEMA_MEMBER(label)
PYLABHUB_SCHEMA_END(pylabhub::tests::TestDataBlock)

PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::MinimalData)
    PYLABHUB_SCHEMA_MEMBER(id)
PYLABHUB_SCHEMA_END(pylabhub::tests::MinimalData)

PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::LargeTestData)
    PYLABHUB_SCHEMA_MEMBER(id)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(payload)
PYLABHUB_SCHEMA_END(pylabhub::tests::LargeTestData)

PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::FrameMeta)
    PYLABHUB_SCHEMA_MEMBER(frame_number)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
PYLABHUB_SCHEMA_END(pylabhub::tests::FrameMeta)

PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::SensorData)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(temperature)
    PYLABHUB_SCHEMA_MEMBER(humidity)
    PYLABHUB_SCHEMA_MEMBER(sensor_id)
PYLABHUB_SCHEMA_END(pylabhub::tests::SensorData)
