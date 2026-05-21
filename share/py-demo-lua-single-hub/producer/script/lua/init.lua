-- Lua demo producer.  Single-hub SHM.  Writes count + value per slot
-- at 50 Hz; calls api.stop() after SLOT_TARGET slots so the close
-- cascade tears the pipeline down cleanly.

local SLOT_TARGET = 200

local count = 0
local stopped = false

function on_init(api)
    api.log("info", string.format(
        "LuaProd started uid=%s channel=%s target=%d",
        api.uid(), api.channel(), SLOT_TARGET))
end

function on_produce(tx, msgs, api)
    if tx.slot == nil then
        return false
    end
    count = count + 1
    tx.slot.count = count
    tx.slot.value = math.sin(count * 0.1) * 100.0
    if count % 50 == 0 then
        api.log("info", string.format(
            "LuaProd wrote count=%d value=%.4f",
            count, tx.slot.value))
    end
    if (not stopped) and count >= SLOT_TARGET then
        stopped = true
        api.log("info", string.format(
            "LuaProd reached target=%d — api.stop() to begin close cascade",
            SLOT_TARGET))
        api.stop()
    end
    return true
end

function on_stop(api)
    api.log("info", string.format("LuaProd STOPPED total=%d", count))
end
