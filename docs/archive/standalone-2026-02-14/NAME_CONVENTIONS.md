# DataBlock Producer/Consumer Name Conventions

**Purpose:** Define how display names are formed and how to compare names for channel/broker lookup. The runtime appends a **suffix** to provide context (process and instance); this is **not** in the hot path—computed once per instance and stored.

---

## 1. Display name format

`DataBlockProducer::name()` and `DataBlockConsumer::name()` return a **display name**:

| Case | Format | Example |
|------|--------|--------|
| No pImpl (null/moved-from) | `"(null)"` | `(null)` |
| User provided a name at creation | `<user_name> \| pid:<pid>-<idx>` | `sensor_channel \| pid:12345-0` |
| No name provided at creation | `producer-<pid>-<idx>` or `consumer-<pid>-<idx>` | `producer-12345-1` |

- **Suffix marker:** The substring **` | pid:`** (space, vertical bar, space, then `pid:`) starts the runtime suffix. Everything from that point to the end is **not** part of the user-assigned name.
- **&lt;pid&gt;** is the process id; **&lt;idx&gt;** is a process-local instance index (unique per producer/consumer instance).
- Display name is **computed once** when `name()` is first called (via `std::call_once`) and **cached**; subsequent calls return the same constant reference. No per-call allocation or formatting.

---

## 2. Rule: user names must not contain the suffix marker

**User-assigned names (channel name, DataBlock name) must not contain the literal substring ` | pid:`** (space, pipe, space, `pid:`). This ensures the runtime can reliably split “logical name” vs “suffix” for comparison and lookup.

- Allowed: `sensor_channel`, `my-block`, `channel_1`, any name without ` | pid:`.
- Disallowed: `foo | pid:123` (would be parsed as logical name `foo` and suffix ` | pid:123`).

---

## 3. Comparing and looking up names

For **channel lookup**, **broker registration/discovery**, or **equality of two producers/consumers**, compare only the **logical name** (the part before the suffix), not the full display string.

Use the helper:

```cpp
std::string_view logical_name(std::string_view full_name) noexcept;
```

- **Input:** The full string from `producer.name()` or `consumer.name()` (or any string that may include the suffix).
- **Output:** The substring before the first occurrence of ` | pid:`, or the whole string if the suffix marker is not present.

Example:

```cpp
auto a = producer->name();
auto b = other_producer->name();
if (pylabhub::hub::logical_name(a) == pylabhub::hub::logical_name(b)) {
    // same channel / logical name
}
```

---

## 4. Summary

| Use | Action |
|-----|--------|
| Logging / diagnostics | Use full `name()` (includes suffix for context). |
| Channel or broker comparison / lookup | Use `logical_name(name())` or compare `logical_name(a) == logical_name(b)`. |
| Choosing user-assigned names | Do **not** use the substring ` \| pid:` in names. |
| Performance | `name()` is not hot path; display name is computed once per instance and stored. |

See **`data_block.hpp`** for `logical_name()` and the `name()` API.
