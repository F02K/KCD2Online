events.on("hello", function(payload)
    print(payload.message)
    events.emit_server("hello_ack", { ok = true })
end)
