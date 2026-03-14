#pragma once
/**
 * @file data_block_fwd.hpp
 * @brief Forward declarations for DataBlock classes and key enums.
 *
 * Include this header when only pointer or reference types to DataBlock objects
 * are needed, avoiding the full data_block.hpp include. Consumers that call
 * member functions on DataBlockProducer or DataBlockConsumer must include the
 * full data_block.hpp.
 *
 * Includes data_block_config.hpp for the complete set of enums and DataBlockConfig,
 * which are value types used in function signatures.
 */
#include "utils/data_block_config.hpp"

namespace pylabhub::hub
{

// ── Forward declarations for pImpl classes ────────────────────────────────────
class DataBlockProducer;
class DataBlockConsumer;
class SlotWriteHandle;
class SlotConsumeHandle;

// Diagnostic handle — rarely needed but forward-declared for completeness.
class DataBlockDiagnosticHandle;

} // namespace pylabhub::hub
