server.on("player_joined", function(player)
    events.emit_client(player.id, "hello", {
        message = "Hello " .. player.name,
        protocol = 1
    })
end)

events.on("hello_ack", function(player_id, payload)
    if type(payload) ~= "table" or payload.ok ~= true then
        return
    end

    server.say("Client Lua is active.", player_id)
end)
