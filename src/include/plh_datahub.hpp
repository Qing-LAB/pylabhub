#pragma once
/**
 * @file plh_datahub.hpp
 * @brief Layer 3 umbrella — full DataHub API (producers, consumers, broker, config).
 *
 * **What is exposed** (all three hub roles):
 *   - Client API: DataBlock SHM, ProducerOptions/ConsumerOptions (consumed
 *                 by RoleAPIBase::build_tx_queue / build_rx_queue),
 *                 QueueReader/QueueWriter abstract handles
 *   - Server API: BrokerService, JsonConfig, HubConfig, ChannelAccessPolicy
 *   - Schema:     schema_blds (BLDS layout validation)
 *
 * **Include cost**: plh_service.hpp (lifecycle + logger + filelock + crypto).
 * Use individual component headers when you need only a subset:
 *   - DataBlock SHM only:  utils/data_block.hpp
 *   - Queue options (role host construction): utils/hub_producer.hpp or
 *     utils/hub_consumer.hpp — these now expose only the options structs
 *     (the hub::Producer/Consumer classes were removed in L3.γ A6.3)
 *   - Server (hub admin):  utils/broker_service.hpp + utils/hub_config.hpp
 */
#include "plh_service.hpp"

#include "utils/schema_blds.hpp"
#include "utils/json_config.hpp"
#include "utils/broker_service.hpp"
#include "utils/data_block.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_config.hpp"
#include "utils/channel_access_policy.hpp"
#include "utils/channel_pattern.hpp"
#include "utils/data_block_mutex.hpp"
