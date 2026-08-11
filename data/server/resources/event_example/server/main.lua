server.on("player_joined", function(player)
    events.emit_client(player.id, "hello", { message = "Hallo " .. player.name })
end)

events.on("hello_ack", function(player_id, payload)
    server.say("Client-Lua von Spieler " .. player_id .. " ist aktiv.")
end)
