/**
 * @file schema_example.cpp
 * @brief Example demonstrating BLDS schema generation and validation.
 *
 * This example shows how to:
 * 1. Define a struct for DataBlock
 * 2. Register it with PYLABHUB_SCHEMA_*() macros
 * 3. Generate SchemaInfo with BLDS string and hash
 * 4. Validate schema compatibility
 */
#include "plh_datahub.hpp"
#include <iostream>
#include <iomanip>

// ============================================================================
// Example: Sensor Data Structure
// ============================================================================

struct SensorData
{
    uint64_t timestamp_ns;
    float temperature;
    float pressure;
    float humidity;
};

// Register schema for SensorData
PYLABHUB_SCHEMA_BEGIN(SensorData)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(temperature)
    PYLABHUB_SCHEMA_MEMBER(pressure)
    PYLABHUB_SCHEMA_MEMBER(humidity)
PYLABHUB_SCHEMA_END(SensorData)

// ============================================================================
// Example: Configuration Structure with Arrays
// ============================================================================

struct SystemConfig
{
    uint32_t config_version;
    char device_name[64];
    uint8_t mac_address[6];
    float calibration_coefficients[4];
};

// Register schema for SystemConfig
PYLABHUB_SCHEMA_BEGIN(SystemConfig)
    PYLABHUB_SCHEMA_MEMBER(config_version)
    PYLABHUB_SCHEMA_MEMBER(device_name)
    PYLABHUB_SCHEMA_MEMBER(mac_address)
    PYLABHUB_SCHEMA_MEMBER(calibration_coefficients)
PYLABHUB_SCHEMA_END(SystemConfig)

// ============================================================================
// Helper: Print Schema Info
// ============================================================================

void print_schema_info(const pylabhub::schema::SchemaInfo& schema)
{
    std::cout << "\n=== Schema Info ===" << std::endl;
    std::cout << "Name:    " << schema.name << std::endl;
    std::cout << "Version: " << schema.version.to_string() << std::endl;
    std::cout << "Size:    " << schema.struct_size << " bytes" << std::endl;
    std::cout << "BLDS:    " << schema.blds << std::endl;
    std::cout << "Hash:    ";
    for (size_t i = 0; i < 32; ++i) {
        std::cout << std::hex << std::setfill('0') << std::setw(2)
                  << static_cast<int>(schema.hash[i]);
    }
    std::cout << std::dec << std::endl;
}

// ============================================================================
// Main: Demonstrate Schema Generation
// ============================================================================

int main()
{
    std::cout << "PyLabHub BLDS Schema Generation Example\n" << std::endl;

    // Example 1: SensorData schema
    auto sensor_schema = pylabhub::schema::generate_schema_info<SensorData>(
        "SensorHub.SensorData",
        pylabhub::schema::SchemaVersion{1, 0, 0}
    );
    print_schema_info(sensor_schema);

    // Example 2: SystemConfig schema
    auto config_schema = pylabhub::schema::generate_schema_info<SystemConfig>(
        "DeviceManager.SystemConfig",
        pylabhub::schema::SchemaVersion{2, 1, 0}
    );
    print_schema_info(config_schema);

    // Example 3: Schema validation (matching)
    std::cout << "\n=== Schema Validation ===" << std::endl;
    auto sensor_schema2 = pylabhub::schema::generate_schema_info<SensorData>(
        "SensorHub.SensorData",
        pylabhub::schema::SchemaVersion{1, 0, 0}
    );

    if (sensor_schema.matches(sensor_schema2)) {
        std::cout << "✓ Schemas match (same hash)" << std::endl;
    } else {
        std::cout << "✗ Schemas don't match!" << std::endl;
    }

    // Example 4: Schema validation (mismatched)
    try {
        pylabhub::schema::validate_schema_match(
            sensor_schema,
            config_schema,
            "Producer/Consumer schema check"
        );
        std::cout << "✗ Validation should have failed!" << std::endl;
    } catch (const pylabhub::schema::SchemaValidationException& ex) {
        std::cout << "✓ Schema mismatch detected correctly: " << ex.what() << std::endl;
    }

    std::cout << "\n=== Example Complete ===" << std::endl;
    return 0;
}
