# Timers and modules

[Back to scripting overview](../README.md)

Server resources can schedule bounded callbacks and split code into local Lua modules. Client resources support modules but currently have no timer API.

## One-shot timers

```lua
timer.after(5000, function()
    server.say("Five seconds have passed.")
end)
```

`timer.after(delay_ms, callback)` returns a numeric timer ID. The delay must be between `1` and `86400000` milliseconds (24 hours).

## Repeating timers

```lua
local count = 0

timer.every(60000, function()
    count = count + 1
    print("Minute tick", count)
end)
```

`timer.every(interval_ms, callback)` returns a numeric timer ID and repeats until the resource stops or is disabled. There is currently no timer cancellation API, so avoid creating unbounded or per-frame repeating timers.

Timers run on the server resource runtime, not on a real-time thread. A callback runs on the first resource tick after its due time; exact millisecond scheduling is not guaranteed.

## Capture player IDs carefully

```lua
server.on("player_joined", function(player)
    local player_id = player.id
    timer.after(10000, function()
        for _, current in ipairs(server.players()) do
            if current.id == player_id and current.connected then
                server.say("Still connected after ten seconds.", player_id)
                return
            end
        end
    end)
end)
```

A player may disconnect before a timer fires. Recheck current state inside delayed callbacks.

## Modules

Given `server/lib/messages.lua`:

```lua
local M = {}

function M.welcome(name)
    return "Welcome, " .. name .. "!"
end

return M
```

Load it from `server/main.lua`:

```lua
local messages = require("lib.messages")

server.on("player_joined", function(player)
    server.say(messages.welcome(player.name), player.id)
end)
```

On the client, the same module name resolves below `client/`. Modules cannot cross the server/client boundary and cannot load from another resource.

## Available Lua libraries

The sandbox opens Lua's base, `table`, `string`, and `math` libraries. It replaces `print` with resource-prefixed logging and provides the restricted `require` described above. File, OS, package, debug, network, and native-library access are unavailable.
