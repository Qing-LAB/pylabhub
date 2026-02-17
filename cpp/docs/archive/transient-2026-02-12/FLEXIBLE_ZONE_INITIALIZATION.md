# Flexible Zone Initialization – Logical Flow and Verification

**Purpose:** Clarify how flexible zone info is initialized across all paths (creator, attacher with/without expected_config) and verify that no init path "hangs in the air" (leaves state uninitialized or inconsistent).

---

## 1. Data Structures and Ownership

| Structure | Holder | Type | Purpose |
|-----------|--------|------|---------|
| `FlexibleZoneConfig` | `DataBlockConfig::flexible_zone_configs` | `vector<FlexibleZoneConfig>` | User input: name, size, spinlock_index per zone |
| `FlexibleZoneInfo` | `DataBlock::m_flexible_zone_info` | `map<name, FlexibleZoneInfo>` | Offset + size + spinlock per zone. Used by checksum impl and index-by-iteration |
| `FlexibleZoneInfo` | `DataBlockProducerImpl::flexible_zones_info` | `vector<FlexibleZoneInfo>` | Index-based access for Producer, SlotWriteHandle |
| `FlexibleZoneInfo` | `DataBlockConsumerImpl::flexible_zones_info` | `vector<FlexibleZoneInfo>` | Index-based access for Consumer, SlotConsumeHandle |
| `flexible_zone_size` | `SharedMemoryHeader` | `size_t` | Total flexible zone bytes (layout) |
| `flexible_zone_offset` | `DataBlockLayout` | `size_t` | Byte offset of flexible zone in shm |

**Single source for offset calculation:** `build_flexible_zone_info(configs)` — computes `offset = sum(previous sizes)` for each zone.

---

## 2. Flow Chart – Creator Path (Producer)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ CREATOR PATH: create_datablock_producer(hub, name, policy, config)          │
└─────────────────────────────────────────────────────────────────────────────┘

  DataBlockConfig                    create_datablock_producer_impl()
  ┌──────────────────────┐
  │ flexible_zone_configs│
  │ [{name,size,spinlock}]│
  └──────────┬───────────┘
             │
             ▼
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ DataBlock(name, config)  [creator ctor]                                   │
  │   • m_layout = DataBlockLayout::from_config(config)                       │
  │     → flexible_zone_size = config.total_flexible_zone_size()              │
  │   • m_flexible_data_zone = shm.base + m_layout.flexible_zone_offset       │
  │   • vec = build_flexible_zone_info(config.flexible_zone_configs)          │
  │   • m_flexible_zone_info[name] = vec[i]  for each zone                    │
  └──────────────────────────────────────────────────────────────────────────┘
             │
             │   impl->dataBlock = new DataBlock(name, config)
             ▼
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ DataBlockProducerImpl                                                     │
  │   • flexible_zones_info = build_flexible_zone_info(config.flexible_zone_  │
  │       configs)                                                            │
  └──────────────────────────────────────────────────────────────────────────┘

  RESULT: DataBlock.m_flexible_zone_info ✓   ProducerImpl.flexible_zones_info ✓
```

---

## 3. Flow Chart – Attacher Path (Consumer)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ ATTACHER PATH: find_datablock_consumer(hub, name, secret, expected_config?) │
└─────────────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────────┐
  │ DataBlock(name)  [attacher ctor]                                          │
  │   • shm_attach(name)                                                      │
  │   • m_layout = DataBlockLayout::from_header(header)                       │
  │     → flexible_zone_size from header->flexible_zone_size                  │
  │   • m_flexible_data_zone = shm.base + m_layout.flexible_zone_offset       │
  │   • m_flexible_zone_info = EMPTY  (not populated in ctor)                 │
  └──────────────────────────────────────────────────────────────────────────┘
             │
             │   impl->dataBlock = new DataBlock(name)
             ▼
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ find_datablock_consumer_impl()                                            │
  │   if (expected_config) {                                                  │
  │     validate header vs expected_config                                    │
  │     impl->flexible_zones_info = build_flexible_zone_info(configs)         │
  │     impl->dataBlock->set_flexible_zone_info_for_attach(configs)           │
  │   } else {                                                                │
  │     impl->flexible_zones_info = EMPTY                                     │
  │     DataBlock.m_flexible_zone_info = EMPTY (unchanged)                    │
  │   }                                                                       │
  └──────────────────────────────────────────────────────────────────────────┘

  WITH expected_config:  DataBlock.m_flexible_zone_info ✓   ConsumerImpl.flexible_zones_info ✓
  WITHOUT expected_config: both EMPTY → flexible_zone_span() returns empty, verify returns false
```

---

## 4. API → Data Source Matrix

| API | Reads From | When Empty / Invalid |
|-----|------------|----------------------|
| `DataBlockProducer::flexible_zone_span(index)` | `ProducerImpl::flexible_zones_info[index]` | index >= size → empty span |
| `DataBlockConsumer::flexible_zone_span(index)` | `ConsumerImpl::flexible_zones_info[index]` | index >= size → empty span |
| `SlotWriteHandle::flexible_zone_span(index)` | `owner->flexible_zones_info[index]` (ProducerImpl) | index >= size → empty span |
| `SlotConsumeHandle::flexible_zone_span(index)` | `owner->flexible_zones_info[index]` (ConsumerImpl) | index >= size → empty span |
| `update_checksum_flexible_zone_impl(block, idx)` | `block->flexible_zone_info()` (DataBlock map) | idx >= map.size → return false |
| `verify_checksum_flexible_zone_impl(block, idx)` | `block->flexible_zone_info()` (DataBlock map) | idx >= map.size → return false |
| `release_write_handle` (checksum update) | `impl.dataBlock->flexible_zone_info()` | loop over map size |
| `release_consume_handle` (checksum verify) | `impl.dataBlock->flexible_zone_info()` | loop over map size |

**Critical invariant:** Checksum impl uses `DataBlock::flexible_zone_info()` (map). Producer/Consumer/SlotHandle span APIs use `Impl::flexible_zones_info` (vector). Both must be populated from the same config when zones are used.

---

## 5. Verification – No Path Hangs in the Air

| Path | DataBlock.m_flexible_zone_info | Impl.flexible_zones_info | Outcome |
|------|-------------------------------|--------------------------|---------|
| Creator (producer) | Populated in ctor via `build_flexible_zone_info` | Populated in factory via `build_flexible_zone_info` | ✓ Both set, span + checksum work |
| Attacher with expected_config | Populated via `set_flexible_zone_info_for_attach` | Populated in factory via `build_flexible_zone_info` | ✓ Both set, span + checksum work |
| Attacher without expected_config | Empty (never set) | Empty (never set) | ✓ Both empty; span returns empty, verify returns false; no UB |
| SlotWriteHandle | N/A | Uses `owner->flexible_zones_info` (ProducerImpl) | ✓ Same as producer |
| SlotConsumeHandle | N/A | Uses `owner->flexible_zones_info` (ConsumerImpl) | ✓ Same as consumer |

**Conclusion:** No path leaves one side populated and the other empty when zones are in use. Without expected_config, both are empty and APIs degrade gracefully (empty span, false verify).

---

## 6. Flow Chart – High-Level Module Dependencies

```
                    ┌─────────────────────────┐
                    │ DataBlockConfig         │
                    │ flexible_zone_configs   │
                    └───────────┬─────────────┘
                                │
                    ┌───────────▼─────────────┐
                    │ build_flexible_zone_info│  ← SINGLE SOURCE for offset calc
                    │ (configs) → vector      │
                    └───────────┬─────────────┘
                                │
              ┌─────────────────┼─────────────────┐
              │                 │                 │
              ▼                 ▼                 ▼
    ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────────────────┐
    │ DataBlock       │ │ ProducerImpl    │ │ ConsumerImpl                │
    │ (creator ctor)  │ │ (factory)       │ │ (factory, if expected_cfg)  │
    │ m_flexible_zone_│ │ flexible_zones_ │ │ flexible_zones_info         │
    │ info[name]=vec  │ │ info = vec      │ │ set_flexible_zone_info_for_ │
    └────────┬────────┘ └────────┬────────┘ │ attach → DataBlock          │
             │                   │          └─────────────┬───────────────┘
             │                   │                        │
             ▼                   ▼                        ▼
    ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────────────────┐
    │ Checksum impl   │ │ Producer::      │ │ Consumer::                  │
    │ update/verify   │ │ flexible_zone_  │ │ flexible_zone_span          │
    │ _flexible_zone  │ │ span            │ │ SlotConsumeHandle::span     │
    └─────────────────┘ │ SlotWriteHandle │ └─────────────────────────────┘
                        │ ::span          │
                        └─────────────────┘
```

---

## 7. Rules (Reference)

1. **Layout:** Flexible zone layout (size, offset) comes only from `FlexibleZoneConfig` list; offset = sum of previous zone sizes. One helper `build_flexible_zone_info(configs)` builds the vector; all init paths use it.
2. **DataBlock (creator):** Ctor populates `m_flexible_zone_info` from `config.flexible_zone_configs` via `build_flexible_zone_info`.
3. **DataBlock (attacher):** Populated only via `set_flexible_zone_info_for_attach(configs)` when the consumer factory has `expected_config`. Without expected_config, it remains empty.
4. **ProducerImpl / ConsumerImpl:** `flexible_zones_info` is set in the factory from the same config that created or validated the block, using `build_flexible_zone_info`. Without expected_config (consumer), it remains empty.
5. **No ad-hoc init:** Do not add offset loops elsewhere; use the helper or the two DataBlock paths above.
