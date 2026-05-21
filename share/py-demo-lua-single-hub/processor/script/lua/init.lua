-- Lua demo processor.  Reads (count, value), writes (count, value, doubled).
-- No explicit shutdown logic — default on_channel_closing handler stops
-- when the input channel closes (producer.stop() fires that cascade).

local processed = 0

function on_init(api)
    api.log("info", string.format(
        "LuaProc started uid=%s %s -> %s",
        api.uid(), api.in_channel(), api.out_channel()))
end

function on_process(rx, tx, msgs, api)
    if rx.slot == nil or tx.slot == nil then
        return false
    end
    tx.slot.count   = rx.slot.count
    tx.slot.value   = rx.slot.value
    tx.slot.doubled = rx.slot.value * 2.0
    processed = processed + 1
    if processed % 50 == 0 then
        api.log("info", string.format(
            "LuaProc processed count=%d value=%.4f doubled=%.4f",
            tx.slot.count, tx.slot.value, tx.slot.doubled))
    end
    return true
end

function on_stop(api)
    api.log("info", string.format(
        "LuaProc STOPPED processed=%d (close-cascade)", processed))
end
