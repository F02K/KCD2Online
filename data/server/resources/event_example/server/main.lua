server.on("player_joined", function(player)
    events.emit_client(player.id, "hello", { message = "Hello " .. player.name })
end)

events.on("hello_ack", function(player_id, payload)
    server.say("Client Lua for player " .. player_id .. " is active.")
end)
