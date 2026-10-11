# Lifecycle events

[Back to scripting overview](../README.md)

Register server lifecycle handlers with `server.on(name, callback)`. Multiple callbacks may be registered for the same event in one resource.

## Event reference

| Event | Callback arguments | When it runs |
| --- | --- | --- |
| `start` | none | After all server resource entry points have loaded successfully |
| `player_joined` | `player` | When an authenticated player has joined |
| `player_left` | `player, reason` | When a player disconnects |
| `chat` | `player_id, text` | When the server processes a player's chat message |
| `player_death` | `player_id` | When the authoritative server processes a death |
| `ui` | `player_id, document_id, control_id, event, payload` | When the built-in client UI sends an interaction; requires `ui` |

The `player` table contains:

```lua
{
    id = 42,
    name = "Player name",
    connected = true,
    role = "player"
}
```

## Startup initialization

```lua
local started_at = 0

server.on("start", function()
    started_at = 1
    print("Resource is ready")
end)
```

Entry-point code executes before `start`. Register callbacks at the top level, then perform startup work in the `start` handler when it should happen after every resource entry point has loaded.

## Join and leave tracking

```lua
local sessions = {}

server.on("player_joined", function(player)
    sessions[player.id] = {
        name = player.name,
        role = player.role
    }
    server.say("Welcome " .. player.name)
end)

server.on("player_left", function(player, reason)
    print(player.name, "left:", reason)
    sessions[player.id] = nil
end)
```

Clean up per-player tables, UI documents, and resource state when a player leaves. The transport also clears the disconnected client's state, but your Lua tables live until the resource stops.

## Chat observation

```lua
server.on("chat", function(player_id, text)
    print("chat", player_id, text)
end)
```

The current callback observes chat; it does not return a cancellation or replacement value. Treat `text` as untrusted input.

## Death handling

```lua
server.on("player_death", function(player_id)
    server.say("You were defeated.", player_id)
end)
```

Use lifecycle events to react to authoritative server state. Do not attempt to reproduce core movement, inventory, or death authority in client Lua.

## Error behavior

An error in one callback is logged with the resource ID. Other resources continue running. When the resource reaches `[resources].error_limit`, that resource is disabled for the remainder of the server process.
