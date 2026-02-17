# Summary: Removing Redundant Non-Template C++ API

## Changes Made

### 1. Created Internal Header
- **`cpp/src/include/utils/detail/data_block_impl.hpp`**: Internal declarations for `create_datablock_producer_impl` and `find_datablock_consumer_impl`. Not exported. Used by data_block.cpp and data_block_recovery.cpp.

### 2. Removed from Public API (data_block.hpp)
**Deleted:**
- Non-template `create_datablock_producer(hub, name, policy, config)`
- Non-template `find_datablock_consumer(hub, name, secret, config)`  
- Non-template `find_datablock_consumer(hub, name, secret)` (no config)
- Exported `create_datablock_producer_impl`
- Exported `find_datablock_consumer_impl`

**Kept:**
- Template `create_datablock_producer<FlexZoneT, DataBlockT>(...)`
- Template `find_datablock_consumer<FlexZoneT, DataBlockT>(..., expected_config)`

### 3. Rationale
- Non-template C++ wrappers were redundant: no types → no schema → just calling impl with nullptr.
- No real use: examples/tests have types and should use template API.
- Recovery needs impl (no types); it will include internal header and call impl directly.
- Future C API will call impl directly (when implemented).

### 4. Next Steps
1. Remove non-template definitions from data_block.cpp
2. Stop exporting impl symbols
3. Update recovery to include internal header
4. Update all examples/tests to use template API
5. Update API docs

## C API Test Policy
**Established `docs/C_API_TEST_POLICY.md`**: C API is the foundation; tests must not be deleted. Protected tests listed; reorganization must preserve coverage.

**Updated docs:**
- `TEST_ORGANIZATION_STRUCTURE.md`: Added mandatory C API test preservation notice
- `CODE_REVIEW_GUIDANCE.md`: Added C API test preservation to review checklist
