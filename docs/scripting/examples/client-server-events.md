# Example: client/server event round trip

[Back to scripting overview](../README.md)

This resource sends a greeting from the server to client Lua. The client prints it and acknowledges receipt. It demonstrates both event directions and therefore creates a downloadable client package.

The same example is included in the server ZIP under `resources/event_example/`.

## Layout

```text
resources/
  event_example/
    resource.toml
    server/
      main.lua
    client/
      main.lua
```

## `resource.toml`

```toml
[resource]
id = "event_example"
version = "1.0.0"
api_version = 1
dependencies = []

[server]
entry = "server/main.lua"
capabilities = ["chat"]

[client]
entry = "client/main.lua"
capabilities = []

[[events]]
name = "hello"
direction = "server_to_client"
reliable = true
max_per_second = 2
max_bytes = 1024

[[events]]
name = "hello_ack"
direction = "client_to_server"
reliable = true
max_per_second = 2
max_bytes = 1024
```

## `server/main.lua`

```lua
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
```

## `client/main.lua`

```lua
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
```

## Message flow

```text
player joins
  -> server player_joined callback
  -> hello event to that player
  -> client hello callback
  -> hello_ack event to server
  -> server hello_ack callback receives authoritative player_id
  -> private system message
```

The acknowledgment proves only that a client sent the event. It does not prove that the official client code ran unchanged. Never use this pattern as anti-cheat or authentication.
