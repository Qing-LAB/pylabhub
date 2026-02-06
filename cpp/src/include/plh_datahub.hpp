#pragma once
/**
 * @file plh_datahub.hpp
 * @brief Layer 3: Data hub modules built on plh_service.
 *
 * Provides JsonConfig, MessageHub, and DataBlock for data storage and exchange.
 * Include this when you need configuration, messaging, or shared memory data blocks.
 */
#include "plh_service.hpp"

#include <nlohmann/json.hpp>

#include "utils/json_config.hpp"
#include "utils/message_hub.hpp"
#include "utils/data_block.hpp"
