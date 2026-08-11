events.on("hello", function(payload)
    if type(payload) ~= "table"
        or type(payload.message) ~= "string"
        or payload.protocol ~= 1 then
        return
    end

    print(payload.message)
    events.emit_server("hello_ack", {
        ok = true,
        protocol = 1
    })
end)
