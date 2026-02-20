#pragma once
/**
 * @file plh_datahub.hpp
 * @brief Layer 3: Data hub modules built on plh_service.
 *
 * Provides the complete Data Exchange Hub API for all three roles:
 *   - Producer: create_datablock_producer / create_channel (Messenger)
 *   - Consumer: find_datablock_consumer / connect_channel (Messenger)
 *   - Broker:   BrokerService (run the central channel discovery hub)
 *
 * Include this single header for full DataBlock, Messenger, ChannelHandle,
 * BrokerService, JsonConfig, and schema validation support.
 */
#include "plh_service.hpp"

#include <nlohmann/json.hpp>

#include "utils/schema_blds.hpp"
#include "utils/json_config.hpp"
#include "utils/broker_service.hpp"
#include "utils/messenger.hpp"
#include "utils/data_block.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_config.hpp"
