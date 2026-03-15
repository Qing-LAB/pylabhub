# DataBlock Metrics API Summary

**Created:** 2026-02-15  
**Purpose:** Quick reference for accessing DataBlock metrics and diagnostics

---

## Overview

The DataBlock provides comprehensive metrics for monitoring, diagnostics, and performance analysis. Metrics are available at three API levels: C API, C++ Producer/Consumer API, and Recovery API.

---

## 1. Metrics Structure

```cpp
struct DataBlockMetrics {
    // === State Snapshot (not reset by reset_metrics) ===
    uint64_t commit_index;           // Last committed slot ID
    uint32_t slot_count;             // Ring buffer capacity
    uint32_t _reserved_metrics_pad;
    
    // === Writer Metrics ===
    uint64_t writer_timeout_count;         // Total writer timeouts
    uint64_t writer_lock_timeout_count;    // Timeouts waiting for write lock
    uint64_t writer_reader_timeout_count;  // Timeouts waiting for readers to drain
    uint64_t writer_blocked_total_ns;      // Total time writer was blocked
    uint64_t write_lock_contention;        // Write lock contention events
    uint64_t write_generation_wraps;       // Generation counter wrap-arounds
    
    // === Reader Metrics ===
    uint64_t reader_not_ready_count;       // Slot not ready for reading
    uint64_t reader_race_detected;         // TOCTTOU races detected
    uint64_t reader_validation_failed;     // Generation validation failures
    uint64_t reader_peak_count;            // Maximum concurrent readers
    
    // === Error Tracking ===
    uint64_t last_error_timestamp_ns;
    uint32_t last_error_code;
    uint32_t error_sequence;
    uint64_t slot_acquire_errors;
    uint64_t slot_commit_errors;
    uint64_t checksum_failures;
    
    // === MessageHub/ZMQ Metrics ===
    uint64_t zmq_send_failures;
    uint64_t zmq_recv_failures;
    uint64_t zmq_timeout_count;
    
    // === Recovery and Schema ===
    uint64_t recovery_actions_count;
    uint64_t schema_mismatch_count;
    
    // === Heartbeat ===
    uint64_t heartbeat_sent_count;
    uint64_t heartbeat_failed_count;
    uint64_t last_heartbeat_ns;
    
    // === Performance Counters ===
    uint64_t total_slots_written;    // Total successful commits
    uint64_t total_slots_read;       // Total slots consumed
    uint64_t total_bytes_written;
    uint64_t total_bytes_read;
    uint64_t uptime_seconds;
    uint64_t creation_timestamp_ns;
};
```

---

## 2. API Levels

### 2.1 C++ Producer/Consumer API (Recommended)

**Use when:** You have an active DataBlockProducer or DataBlockConsumer handle.

```cpp
#include <utils/data_block.hpp>

// From producer
auto producer = create_datablock_producer(...);
DataBlockMetrics metrics;
if (producer->get_metrics(metrics) == 0) {
    std::cout << "Total commits: " << metrics.total_slots_written << "\n";
    std::cout << "Writer timeouts: " << metrics.writer_timeout_count << "\n";
    std::cout << "Has any commits: " << (metrics.total_slots_written > 0) << "\n";
}

// From consumer
auto consumer = find_datablock_consumer(...);
if (consumer->get_metrics(metrics) == 0) {
    std::cout << "Total reads: " << metrics.total_slots_read << "\n";
    std::cout << "Reader races: " << metrics.reader_race_detected << "\n";
    std::cout << "Peak concurrent readers: " << metrics.reader_peak_count << "\n";
}

// Reset metrics (e.g., after resolving issues)
producer->reset_metrics();  // Counters reset to 0, state preserved
```

**Characteristics:**
- ✅ Most efficient (uses existing handle)
- ✅ Thread-safe
- ✅ No exceptions
- ✅ Returns -1 on error

### 2.2 C API (Low-Level)

**Use when:** You have direct access to SharedMemoryHeader, or writing C bindings.

```cpp
#include <utils/slot_rw_coordinator.h>

SharedMemoryHeader* header = ...; // Your access method
DataBlockMetrics metrics;
if (slot_rw_get_metrics(header, &metrics) == 0) {
    printf("Commit index: %llu\n", metrics.commit_index);
    printf("Has commits: %s\n", metrics.total_slots_written > 0 ? "yes" : "no");
}

// Reset metrics
slot_rw_reset_metrics(header);
```

**Characteristics:**
- ✅ No C++ runtime dependencies
- ✅ Direct header access
- ✅ Used internally by C++ API
- ⚠️ Requires SharedMemoryHeader pointer

### 2.3 Recovery API (Name-Based)

**Use when:** You need to inspect metrics from external diagnostic tools without active handles.

```cpp
#include <utils/recovery_api.hpp>

// Query by DataBlock name
DataBlockMetrics metrics;
if (datablock_get_metrics("my_datablock", &metrics) == 0) {
    printf("DataBlock 'my_datablock' metrics:\n");
    printf("  Commit index: %llu\n", metrics.commit_index);
    printf("  Total commits: %llu\n", metrics.total_slots_written);
    printf("  Writer timeouts: %llu\n", metrics.writer_timeout_count);
}

// Reset by name
datablock_reset_metrics("my_datablock");
```

**Characteristics:**
- ✅ No handle required
- ✅ Suitable for CLI tools, monitoring scripts
- ⚠️ Opens/closes DataBlock internally (not for hot path)
- ⚠️ Less efficient than Producer/Consumer API

---

## 3. Common Use Cases

### 3.1 Check if Any Data Has Been Written

```cpp
DataBlockMetrics metrics;
producer->get_metrics(metrics);

if (metrics.total_slots_written == 0) {
    std::cout << "No data written yet\n";
} else {
    std::cout << "Data present: " << metrics.total_slots_written << " commits\n";
}
```

**Critical for checksum validation:** Don't validate checksums if no commits yet!

### 3.2 Monitor Performance

```cpp
DataBlockMetrics before, after;
producer->get_metrics(before);

// ... run workload ...

producer->get_metrics(after);

uint64_t commits = after.total_slots_written - before.total_slots_written;
uint64_t timeouts = after.writer_timeout_count - before.writer_timeout_count;

std::cout << "Throughput: " << commits << " commits\n";
std::cout << "Timeout rate: " << (timeouts * 100.0 / commits) << "%\n";
```

### 3.3 Detect Issues

```cpp
DataBlockMetrics metrics;
consumer->get_metrics(metrics);

// Check for reader-writer coordination issues
if (metrics.reader_race_detected > 0) {
    LOGGER_WARN("TOCTTOU races detected: {}", metrics.reader_race_detected);
}

if (metrics.reader_validation_failed > 0) {
    LOGGER_WARN("Reader validation failures: {}", metrics.reader_validation_failed);
}

// Check for checksum integrity issues
if (metrics.checksum_failures > 0) {
    LOGGER_ERROR("Checksum failures detected: {}", metrics.checksum_failures);
}
```

### 3.4 Health Monitoring

```cpp
// Periodic health check
void check_datablock_health(DataBlockProducer* producer) {
    DataBlockMetrics metrics;
    if (producer->get_metrics(metrics) != 0) {
        LOGGER_ERROR("Failed to get metrics");
        return;
    }
    
    // Check timeout rates
    double timeout_rate = (double)metrics.writer_timeout_count / 
                         std::max(1ULL, metrics.total_slots_written);
    if (timeout_rate > 0.05) {  // > 5% timeout rate
        LOGGER_WARN("High timeout rate: {:.2f}%", timeout_rate * 100);
    }
    
    // Check lock contention
    if (metrics.write_lock_contention > 100) {
        LOGGER_WARN("High lock contention: {}", metrics.write_lock_contention);
    }
    
    // Check for errors
    if (metrics.slot_acquire_errors > 0 || metrics.slot_commit_errors > 0) {
        LOGGER_ERROR("Slot operation errors - acquire: {}, commit: {}",
                    metrics.slot_acquire_errors, metrics.slot_commit_errors);
    }
}
```

---

## 4. Internal Access Functions (Phase 1)

For implementation code in `data_block.cpp`, use centralized access functions in `detail` namespace:

```cpp
// In detail namespace (data_block.cpp)

// Metrics (hot path - increment on events)
detail::increment_metric_writer_timeout(header);
detail::increment_metric_total_commits(header);
detail::has_any_commits(header);  // For validation logic

// Indices (coordination)
detail::get_commit_index(header);
detail::increment_commit_index(header);
detail::get_write_index(header);

// Config (read-only)
detail::get_consumer_sync_policy(header);
detail::get_ring_buffer_capacity(header);
```

**Benefits:**
- ✅ Consistent null pointer handling
- ✅ Correct memory ordering enforced
- ✅ Single point of maintenance
- ✅ Foundation for validation hooks

---

## 5. Authorization (Future Enhancement)

**Current State:** Metrics are readable by anyone with DataBlock access.

**Planned for Phase 4:**
- Diagnostic handle with shared_secret validation
- MessageHub-mediated metrics requests
- Broker audit trail for metrics access
- Read-only vs read-write permissions

**For now:** Use OS-level permissions (file system access control) to restrict who can open DataBlocks.

---

## 6. Best Practices

### 6.1 Monitoring

✅ **DO:**
- Use `get_metrics()` for periodic monitoring
- Check `has_any_commits()` before validating checksums
- Monitor timeout rates and contention metrics
- Track peak reader count for capacity planning

❌ **DON'T:**
- Call `get_metrics()` in hot path (it's lightweight but not zero-cost)
- Reset metrics in production without logging current state
- Ignore high timeout or contention rates

### 6.2 Diagnostics

✅ **DO:**
- Use Recovery API (`datablock_get_metrics`) for external tools
- Log metrics on errors or incidents
- Include metrics in crash reports
- Compare metrics before/after changes

❌ **DON'T:**
- Open diagnostic handles in tight loops
- Assume metrics are perfectly synchronized (they use relaxed ordering)
- Rely on metrics for correctness (use for diagnostics only)

---

## 7. Related Documentation

- **C API Details:** `src/include/utils/slot_rw_coordinator.h`
- **Recovery API:** `src/include/utils/recovery_api.hpp`
- **Implementation:** `src/utils/data_block.cpp` (detail namespace access functions)
- **Testing:** `tests/test_layer2_service/test_slot_rw_coordinator.cpp`
- **Design:** `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`

---

**Document Status:** Complete  
**Last Updated:** 2026-02-15 (Phase 1 completion)
