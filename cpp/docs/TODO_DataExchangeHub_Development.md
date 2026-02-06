# Data Exchange Hub (DataBlock & MessageHub) - Development TODO

This document consolidates the findings from a comprehensive evaluation of the `DataBlock` and `MessageHub` modules, identifying test coverage gaps, critical missing implementations, and future development steps towards a robust "Data Exchange Hub".

## Table of Contents
1.  [Cross-Platform Shared Memory Mutex](#1-cross-platform-shared-memory-mutex)
    1.1 [Define `SharedSpinLockState`](#11-define-sharedspinlockstate)
    1.2 [Implement `SharedSpinLock` Class](#12-implement-sharedspinlock-class)
    1.3 [Implement `SharedSpinLockGuard`](#13-implement-sharedspinlockguard)
    1.4 [Implement `DataBlockMutex` & `DataBlockLockGuard` (Internal Management Mutex)](#14-implement-datablockmutex--datablocklockguard-internal-management-mutex)
    1.5 [Integrate `DataBlockMutex` into `DataBlock`](#15-integrate-datablockmutex-into-datablock)
    1.6 [Add Tests for Cross-Process Mutex (Internal)](#16-add-tests-for-cross-process-mutex-internal)
    1.7 [Add Tests for `SharedSpinLock` (User-Facing)](#17-add-tests-for-sharedspinlock-user-facing)
2.  [DataBlock Core Functionality](#2-datablock-core-functionality)
    2.1 [Integrate `SharedSpinLock` into `DataBlockProducerImpl` and `DataBlockConsumerImpl`](#21-integrate-sharedspinlock-into-datablockproducerimpl-and-datablockconsumerimpl)
    2.2 [Implement Data Transfer Methods](#22-implement-data-transfer-methods)
    2.3 [Add Tests for Data Transfer](#23-add-tests-for-data-transfer)
    2.4 [Add Tests for Concurrent Access](#24-add-tests-for-concurrent-access)
    2.5 [Add Tests for Boundary Conditions](#25-add-tests-for-boundary-conditions)
    2.6 [Add Tests for Ring Buffer Logic](#26-add-tests-for-ring-buffer-logic)
    2.7 [Add Tests for Structured Buffer Discovery](#27-add-tests-for-structured-buffer-discovery)
3.  [DataBlock Shared Memory Management](#3-datablock-shared-memory-management)
    3.1 [Add Tests for Concurrent Producer Creation](#31-add-tests-for-concurrent-producer-creation)
    3.2 [Add Tests for Producer/Consumer Race Conditions](#32-add-tests-for-producerconsumer-race-conditions)
    3.3 [Add Tests for Invalid Names](#33-add-tests-for-invalid-names)
    3.4 [Add Tests for Producer Crash Cleanup](#34-add-tests-for-producer-crash-cleanup)
    3.5 [Add Tests for `DATABLOCK_VERSION` Mismatch](#35-add-tests-for-datablock_version-mismatch)
    3.6 [Add Negative Tests for Header Mismatches](#36-add-negative-tests-for-header-mismatches)
4.  [DataBlock Lifecycle & Cleanup](#4-datablock-lifecycle--cleanup)
    4.1 [Add Tests for Orphaned Segment Cleanup](#41-add-tests-for-orphaned-segment-cleanup)
    4.2 [Add Tests for `active_consumer_count`](#42-add-tests-for-active_consumer_count)
    4.3 [Add Tests for Producer Unlinking with Active Consumers](#43-add-tests-for-producer-unlinking-with-active-consumers)
    4.4 [Add Tests for Consumer Destructor on Failed Construction](#44-add-tests-for-consumer-destructor-on-failed-construction)
5.  [MessageHub Advanced Scenarios](#5-messagehub-advanced-scenarios)
    5.1 [Add Tests for Concurrent `send_request`/`send_notification`](#51-add-tests-for-concurrent-send_requestsend_notification)
    5.2 [Add Tests for Broker Unavailable/Crash](#52-add-tests-for-broker-unavailablecrash)
    5.3 [Add Tests for Large Payloads](#53-add-tests-for-large-payloads)
    5.4 [Add Tests for Network Intermittency/Recovery](#54-add-tests-for-network-intermittencyrecovery)
    5.5 [Add Negative Tests for Malformed Broker Responses](#55-add-negative-tests-for-malformed-broker-responses)
    5.6 [Add Stress Tests for MessageHub](#56-add-stress-tests-for-messagehub)
    5.7 [Add Tests for Resource Leaks on Reconnect](#57-add-tests-for-resource-leaks-on-reconnect)
    5.8 [Add Tests for `zmq_curve_keypair` Failure](#58-add-tests-for-zmq_curve_keypair-failure)
    5.9 [Add Tests for ZMQ `ZMQ_MAXMSGSIZE` Exceedance](#59-add-tests-for-zmq-zmq_maxmsgsize-exceedance)
6.  [Overall Data Hub Integration (DataBlock & MessageHub)](#6-overall-data-hub-integration-datablock--messagehub)
    6.1 [Implement Broker Registration Protocol](#61-implement-broker-registration-protocol)
    6.2 [Implement Coordinated Data Transfer](#62-implement-coordinated-data-transfer)
    6.3 [Add End-to-End Tests for Registration/Discovery](#63-add-end-to-end-tests-for-registrationdiscovery)
    6.4 [Add End-to-End Tests for Coordinated Data Transfer](#64-add-end-to-end-tests-for-coordinated-data-transfer)
    6.5 [Add End-to-End Tests for Error Handling/Recovery](#65-add-end-to-end-tests-for-error-handlingrecovery)
    6.6 [Add End-to-End Tests for Performance/Scalability](#66-add-end-to-end-tests-for-performancescalability)

---

## 1. Cross-Platform Shared Memory Mutex

**Background**: The Data Exchange Hub requires robust cross-process synchronization for managing shared memory resources. Initially, the goal was to leverage `AtomicGuard`, but its design proved unsuitable for cross-process synchronization due to its reliance on intra-process `std::atomic` semantics. The revised approach adopts a two-tiered locking mechanism to balance robustness with the constraint of using only shared memory content for user-facing locks.

**Goal**: Implement a robust, cross-platform synchronization system for shared memory that respects the constraint of operating strictly within the assigned memory block for user-facing locks, while using robust OS-provided mutexes for internal management of these locks.

**Tiered Approach**:
*   **Tier 1 (User-Facing Locks - `SharedSpinLock`)**: These are atomic-based spin-locks implemented purely within the shared memory block, designed for direct use by consumers of the DataBlock for data coordination. They will be simple, fixed-size, and language-agnostic.
*   **Tier 2 (Internal Management Mutex - `DataBlockMutex`)**: A single, robust, OS-specific mutex (e.g., `pthread_mutex_t` on POSIX, named kernel mutex on Windows) used to protect the allocation and deallocation of the Tier 1 `SharedSpinLock` instances and other critical metadata within the `SharedMemoryHeader`.

### 1.1 Define `SharedSpinLockState`
*   **Description**: Define the `SharedSpinLockState` struct within `SharedMemoryHeader`. This struct will contain `std::atomic<uint64_t> owner_pid`, `std::atomic<uint64_t> generation`, `std::atomic<uint32_t> recursion_count`, and `uint64_t owner_thread_id`. These atomic variables will form the core of the user-facing spin-locks.
*   **Status**: COMPLETED.
*   **Procedure**: See `src/include/utils/DataBlock.hpp`.

### 1.2 Implement `SharedSpinLock` Class
*   **Description**: Design and implement the `SharedSpinLock` class. This class will wrap a pointer to a `SharedSpinLockState` struct in shared memory and implement the core `lock()`, `unlock()`, and `try_lock_for()` logic. This includes robust handling of dead PID detection, generation counters to mitigate PID reuse, and support for recursive locking by the same thread.
*   **Status**: COMPLETED.
*   **Procedure**: See `src/include/utils/shared_spin_lock.hpp` and `src/utils/shared_spin_lock.cpp`.

### 1.3 Implement `SharedSpinLockGuard`
*   **Description**: Design and implement a RAII `SharedSpinLockGuard` for the `SharedSpinLock` class, ensuring automatic acquisition and release of the spin-lock upon scope entry and exit.
*   **Status**: COMPLETED.
*   **Procedure**: See `src/include/utils/shared_spin_lock.hpp` and `src/utils/shared_spin_lock.cpp`.

### 1.4 Implement `DataBlockMutex` & `DataBlockLockGuard` (Internal Management Mutex)
*   **Description**: Design and implement the `DataBlockMutex` class (OS-specific, robust cross-process mutex) and its RAII `DataBlockLockGuard`. This mutex protects the allocation map (`SharedMemoryHeader::spinlock_allocated`) and other metadata manipulation within the shared memory.
*   **Status**: COMPLETED.
*   **Procedure**: See `src/include/utils/shared_memory_mutex.hpp` and `src/utils/shared_memory_mutex.cpp`.

### 1.5 Integrate `DataBlockMutex` into `DataBlock`
*   **Description**: Integrate the `DataBlockMutex` into the `DataBlock` internal helper class. This involves:
    *   Adding a `std::unique_ptr<DataBlockMutex> m_management_mutex;` member.
    *   Initializing this member in both the producer and consumer `DataBlock` constructors.
    *   Ensuring `pthread_mutex_destroy` is called correctly for POSIX implementation by explicitly `reset()`ing `m_management_mutex` in `DataBlock::~DataBlock()` before `munmap` and `shm_unlink`.
*   **Status**: COMPLETED.
*   **Procedure**: See `src/utils/DataBlock.cpp`.

### 1.6 Add Tests for Cross-Process Mutex (Internal)
*   **Description**: Develop tests to verify the correctness and robustness of the `DataBlockMutex`. This involves multi-process tests to ensure the management mutex correctly protects access to shared memory metadata.
*   **Status**: PENDING.
*   **Procedure**: Create `tests/test_pylabhub_utils/test_datablock_management_mutex.cpp` (or similar).

### 1.7 Add Tests for `SharedSpinLock` (User-Facing)
*   **Description**: Develop multi-process tests for `SharedSpinLock` and `SharedSpinLockGuard` to verify their contention behavior, robustness to process crashes (dead PID detection and reclamation), and correct recursive locking on a single thread.
*   **Status**: PENDING.
*   **Procedure**: Add to `tests/test_pylabhub_utils/test_datablock.cpp` or create a new test file `test_shared_spin_lock.cpp`.

---

## 2. DataBlock Core Functionality

**Background**: The `DataBlock` provides shared memory regions (`flexible_data_zone`, `structured_data_buffer`) and control mechanisms (`write_index`, `read_index`, `ring_buffer_capacity`), but currently lacks public APIs within `IDataBlockProducer` and `IDataBlockConsumer` to actually transfer data or manage slots. This section focuses on implementing and testing these fundamental data exchange capabilities, leveraging the newly integrated `SharedSpinLock`s.

**Goal**: Implement and test the core data exchange functionality of `DataBlock`, enabling producers to write data to shared buffers and consumers to read from them, with proper synchronization.

### 2.1 Integrate `SharedSpinLock` into `DataBlockProducerImpl` and `DataBlockConsumerImpl`
*   **Description**: Implement the pure virtual methods `acquire_user_spinlock`, `release_user_spinlock` in `DataBlockProducerImpl` and `get_user_spinlock` in `DataBlockConsumerImpl`. These implementations will utilize the `DataBlock` helper class's internal spinlock management (`acquire_shared_spinlock`, `release_shared_spinlock`, `get_shared_spinlock_state`).
*   **Status**: PENDING.
*   **Procedure**: See `src/utils/DataBlock.cpp`.

### 2.2 Implement Data Transfer Methods
*   **Description**: Add pure virtual methods to `IDataBlockProducer` and `IDataBlockConsumer` (e.g., `acquire_write_slot`, `release_write_slot`, `begin_read`, `end_read`, `get_flexible_zone`, `get_structured_buffer`) to enable reading from and writing to the shared memory buffers. Implement these methods in `DataBlockProducerImpl` and `DataBlockConsumerImpl`.
*   **Status**: PENDING.
*   **Procedure**: See `src/include/utils/DataBlock.hpp`, `src/utils/DataBlock.cpp`.

### 2.3 Add Tests for Data Transfer
*   **Description**: Develop tests for fundamental producer write / consumer read operations on both the flexible data zone and the structured data buffer.
*   **Status**: PENDING.
*   **Procedure**: See `tests/test_pylabhub_utils/test_datablock.cpp`.

### 2.4 Add Tests for Concurrent Access
*   **Description**: Once data transfer methods and `SharedSpinLock` are fully integrated, create tests for concurrent read/write access to `DataBlock` buffers from multiple threads/processes, using the `SharedSpinLock`s.
*   **Status**: PENDING.
*   **Procedure**: See `tests/test_pylabhub_utils/test_datablock.cpp`.

### 2.5 Add Tests for Boundary Conditions
*   **Description**: Test edge cases such as writing data that exactly fills a buffer, attempting to write more data than available, writing zero bytes, and reading from empty buffers.
*   **Status**: PENDING.
*   **Procedure**: See `tests/test_pylabhub_utils/test_datablock.cpp`.

### 2.6 Add Tests for Ring Buffer Logic
*   **Description**: Develop specific tests for `DataBlockPolicy::RingBuffer` logic, covering slot management, producer/consumer wrap-around scenarios, queue full/empty conditions, and handling of producer/consumer lag.
*   **Status**: PENDING.
*   **Procedure**: See `tests/test_pylabhub_utils/test_datablock.cpp`.

### 2.7 Add Tests for Structured Buffer Discovery
*   **Description**: Once the mechanism for consumers to discover the layout of the structured buffer (which is currently `nullptr` after initial mapping) is implemented, add tests to verify its correctness.
*   **Status**: PENDING.
*   **Procedure**: See `src/utils/DataBlock.cpp`, `tests/test_pylabhub_utils/test_datablock.cpp`.

---

## 3. DataBlock Shared Memory Management

**Background**: Robust management of shared memory includes handling various creation/connection scenarios, including concurrency and error conditions, to ensure reliable operation.

**Goal**: Ensure `DataBlock` handles shared memory allocation, naming, and initialization robustly under various concurrent and error conditions.

### 3.1 Add Tests for Concurrent Producer Creation
*   **Description**: Test scenarios where multiple processes attempt to `create_datablock_producer` with the same name simultaneously, verifying that only one succeeds or a defined error state is reached. This will involve the `DataBlockMutex` protecting the shared memory creation process.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_datablock.cpp`.

### 3.2 Add Tests for Producer/Consumer Race Conditions
*   **Description**: Test a consumer attempting to connect to a `DataBlock` *before* the producer has fully initialized the `SharedMemoryHeader` (e.g., magic number, version, secret). This requires a mechanism to delay producer initialization slightly.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_datablock.cpp` (multi-process test).

### 3.3 Add Tests for Invalid Names
*   **Description**: Test `DataBlock` creation/opening with invalid shared memory names (e.g., containing illegal characters for OS, excessively long names, empty names).
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_datablock.cpp`.

### 3.4 Add Tests for Producer Crash Cleanup
*   **Description**: Simulate a producer crashing after creating a shared memory segment but before its destructor runs (thus without calling `shm_unlink` on POSIX). Then, verify that a subsequent producer can successfully create a new `DataBlock` with the same name, or that consumers gracefully handle the orphaned segment.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_datablock.cpp` (multi-process test with controlled crash).

### 3.5 Add Tests for `DATABLOCK_VERSION` Mismatch
*   **Description**: Create a `DataBlock` with one version, then attempt to connect a consumer configured with a different `DATABLOCK_VERSION` (simulating an older/newer consumer). Ensure the consumer fails gracefully (e.g., returns `nullptr` or throws a specific exception). This requires implementing the version check in the consumer.
*   **Status**: PENDING.
*   **Procedure**: `src/utils/DataBlock.cpp` (version check implementation), `tests/test_pylabhub_utils/test_datablock.cpp`.

### 3.6 Add Negative Tests for Header Mismatches
*   **Description**: Develop explicit negative tests for `DataBlock` consumers when `magic_number` or `shared_secret` are mismatched. Assert that `find_datablock_consumer` returns `nullptr` for these scenarios.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_datablock.cpp`.

---

## 4. DataBlock Lifecycle & Cleanup

**Background**: Proper lifecycle management and cleanup are crucial to prevent resource leaks and maintain system stability, especially in shared memory contexts.

**Goal**: Ensure `DataBlock` resources are correctly managed throughout their lifecycle, including graceful and robust cleanup under normal and abnormal conditions.

### 4.1 Add Tests for Orphaned Segment Cleanup
*   **Description**: Verify that if a producer crashes and leaves an orphaned shared memory segment, subsequent attempts to create a `DataBlock` with the same name correctly clean up the old segment before creating a new one (as currently intended by `shm_unlink` on POSIX).
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_datablock.cpp` (multi-process test).

### 4.2 Add Tests for `active_consumer_count`
*   **Description**: Verify the correctness of `SharedMemoryHeader::active_consumer_count` under various scenarios:
    *   Incrementing/decrementing with single consumers.
    *   Correctness with multiple concurrent consumers connecting and disconnecting.
    *   Behavior when a consumer crashes (does the count become stale?).
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_datablock.cpp` (multi-process test).

### 4.3 Add Tests for Producer Unlinking with Active Consumers
*   **Description**: Test the behavior when a `DataBlock` producer (creator) destructs and unlinks the shared memory segment while consumers are still actively attached. Confirm that consumers can continue to access the data as long as they hold mappings (POSIX) or that Windows handles this gracefully.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_datablock.cpp` (multi-process test).

### 4.4 Add Tests for Consumer Destructor on Failed Construction
*   **Description**: Test scenarios where `DataBlockConsumerImpl` construction fails (e.g., due to invalid magic number, shared secret, or underlying OS errors). Verify that its destructor (specifically the `m_dataBlock && m_dataBlock->header()` check) handles the potentially null `m_dataBlock` or `header` pointers gracefully without crashing.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_datablock.cpp`.

---

## 5. MessageHub Advanced Scenarios

**Background**: Beyond basic connection and message passing, `MessageHub` needs robust testing for concurrency, resilience to network issues, and handling of various message patterns and edge cases.

**Goal**: Ensure `MessageHub` provides reliable and robust communication under advanced usage patterns and challenging network conditions.

### 5.1 Add Tests for Concurrent `send_request`/`send_notification`
*   **Description**: Test calling `send_request` and `send_notification` concurrently from multiple threads on the same `MessageHub` instance, ensuring thread safety and correct behavior (no crashes, no data corruption, expected return values).
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_messagehub.cpp`.

### 5.2 Add Tests for Broker Unavailable/Crash
*   **Description**: Test `MessageHub`'s behavior when the broker becomes unavailable or crashes during `send_request` (should timeout or fail gracefully) and `send_notification` (should return `false` on send failure, or `true` if locally queued but not delivered).
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_messagehub.cpp` (requires mocking or controlled broker process).

### 5.3 Add Tests for Large Payloads
*   **Description**: Test `MessageHub` with very large JSON payloads for both `send_request` and `send_notification`, verifying performance and absence of failure up to ZMQ's message size limits.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_messagehub.cpp`.

### 5.4 Add Tests for Network Intermittency/Recovery
*   **Description**: Simulate brief network disconnections (e.g., by temporarily stopping the `MockBroker`) and verify `MessageHub`'s behavior. Test successful reconnection via manual `connect()` calls after an outage.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_messagehub.cpp` (requires controlled broker).

### 5.5 Add Negative Tests for Malformed Broker Responses
*   **Description**: Test `MessageHub::send_request`'s handling of malformed responses from the broker (e.g., unexpected number of message parts, invalid header content, corrupted MessagePack payload that doesn't parse as JSON).
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_messagehub.cpp` (requires `MockBroker` to send malformed responses).

### 5.6 Add Stress Tests for MessageHub
*   **Description**: Develop long-running stress tests involving a high volume of `send_request` and `send_notification` calls, checking for memory leaks, deadlocks, and stability.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_messagehub.cpp`.

### 5.7 Add Tests for Resource Leaks on Reconnect
*   **Description**: Verify that repeatedly connecting and disconnecting `MessageHub` (without errors) does not lead to resource leaks (e.g., ZMQ socket handles, contexts).
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_messagehub.cpp`.

### 5.8 Add Tests for `zmq_curve_keypair` Failure
*   **Description**: Test the error path in `MessageHub::connect` if `zmq_curve_keypair` fails (e.g., by mocking or injecting failure) to ensure `connect` returns `false` and logs the error correctly.
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_messagehub.cpp` (requires mocking).

### 5.9 Add Tests for ZMQ `ZMQ_MAXMSGSIZE` Exceedance
*   **Description**: Test `MessageHub`'s behavior when attempting to send a message payload that exceeds ZMQ's configured `ZMQ_MAXMSGSIZE` (if applicable and configurable).
*   **Status**: PENDING.
*   **Procedure**: `tests/test_pylabhub_utils/test_messagehub.cpp`.

---

## 6. Overall Data Hub Integration (DataBlock & MessageHub)

**Background**: The "Data Exchange Hub" concept implies `DataBlock` and `MessageHub` working in concert. This section outlines the need for implementing and testing these higher-level integrated scenarios, which are currently "Missing Components".

**Goal**: Establish a functional and robust Data Exchange Hub by implementing the necessary integration components and validating their behavior with end-to-end tests.

### 6.1 Implement Broker Registration Protocol
*   **Description**: Develop the protocol and implementation for `DataBlock` channels to register themselves with a central broker via `MessageHub`. This would involve defining message types for registration/discovery and implementing the broker-side logic.
*   **Status**: PENDING.
*   **Procedure**: `src/utils/MessageHub.cpp`, new broker module (future), related headers.

### 6.2 Implement Coordinated Data Transfer
*   **Description**: Develop the mechanisms for `MessageHub` to signal data availability or updates within `DataBlock` channels. This involves integrating the `MessageHub` into `IDataBlockProducer`/`IDataBlockConsumer`'s data transfer and slot management methods.
*   **Status**: PENDING.
*   **Procedure**: `src/utils/DataBlock.cpp`, `src/utils/MessageHub.cpp`, related headers.

### 6.3 Add End-to-End Tests for Registration/Discovery
*   **Description**: Develop tests that cover the entire lifecycle of `DataBlock` channel registration and discovery, from a producer registering via `MessageHub` to consumers successfully discovering and connecting to the `DataBlock`.
*   **Status**: PENDING.
*   **Procedure**: New integration test suite.

### 6.4 Add End-to-End Tests for Coordinated Data Transfer
*   **Description**: Develop tests for the complete data transfer workflow: a producer writing data to `DataBlock`, using `MessageHub` to signal, and a consumer reading from `DataBlock` based on the signal.
*   **Status**: PENDING.
*   **Procedure**: New integration test suite.

### 6.5 Add End-to-End Tests for Error Handling/Recovery
*   **Description**: Develop tests for complex error scenarios involving both `DataBlock` and `MessageHub`, such as broker disconnects while `DataBlock` is active, or `DataBlock` unlinking unexpectedly, and verifying graceful recovery or failure.
*   **Status**: PENDING.
*   **Procedure**: New integration test suite.

### 6.6 Add End-to-End Tests for Performance/Scalability
*   **Description**: Develop tests to measure the performance (throughput, latency) and scalability of the integrated Data Hub under various loads and configurations.
*   **Status**: PENDING.
*   **Procedure**: New performance test suite.