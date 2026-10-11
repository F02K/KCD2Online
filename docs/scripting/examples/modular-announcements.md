# Example: modular announcements and timers

[Back to scripting overview](../README.md)

This server-only resource separates formatting into a module, sends a delayed welcome, and broadcasts a repeating announcement.

## Layout

```text
resources/
  announcements/
    resource.toml
    server/
      main.lua
      messages.lua
```

## `resource.toml`

```toml
[resource]
id = "announcements"
version = "1.0.0"
api_version = 1
dependencies = []

[server]
entry = "server/main.lua"
capabilities = ["chat"]
```

## `server/messages.lua`

```lua
local M = {}

function M.welcome(name)
    return "Welcome, " .. name .. ". Please read the server rules."
end

function M.rotation(number)
    return "Server reminder #" .. number .. ": keep authoritative logic on the server."
end

return M
```

## `server/main.lua`

```lua
local messages = require("messages")
local announcement_number = 0

local function player_is_connected(player_id)
    for _, player in ipairs(server.players()) do
        if player.id == player_id and player.connected then
            return true
        end
    end
    return false
end

server.on("player_joined", function(player)
    local player_id = player.id
    local player_name = player.name

    timer.after(3000, function()
        if player_is_connected(player_id) then
            server.say(messages.welcome(player_name), player_id)
        end
    end)
end)

server.on("start", function()
    timer.every(300000, function() -- every five minutes
        announcement_number = announcement_number + 1
        server.say(messages.rotation(announcement_number))
    end)
end)
```

## What this demonstrates

- `require("messages")` resolves only `server/messages.lua`.
- A delayed callback rechecks that its target is still connected.
- The repeating timer is created once during `start`.
- Broadcast `server.say` omits the player ID.

There is currently no timer cancellation API. Do not create a repeating timer per player when a single repeating timer plus a player loop would work.
