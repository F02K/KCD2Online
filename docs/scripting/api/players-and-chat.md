# Players and chat

[Back to scripting overview](../README.md)

The `server` table exposes player enumeration, system messages, and controlled disconnects.

## `server.players()`

Returns a new array containing the currently known players:

```lua
for _, player in ipairs(server.players()) do
    print(player.id, player.name, player.connected, player.role)
end
```

Each entry contains `id`, `name`, `connected`, and `role`. Store the stable numeric `id` for a session instead of using display names as identity.

Example lookup helper:

```lua
local function find_player(player_id)
    for _, player in ipairs(server.players()) do
        if player.id == player_id then
            return player
        end
    end
    return nil
end
```

## `server.say(text [, player_id])`

Requires the `chat` capability.

Send to one player:

```lua
server.say("Only you can see this message.", player_id)
```

Broadcast by omitting the player ID, or by passing `nil` or `0`:

```lua
server.say("The tournament starts in five minutes.")
```

Convert and bound dynamic values before concatenating them. Avoid reflecting raw client input into global messages.

## `server.kick(player_id [, reason])`

Requires the `players.kick` capability.

```lua
server.kick(player_id, "Resource policy violation")
```

The player ID must be positive. If no reason is supplied, the runtime uses `removed by a resource`.

Keep kick decisions server-authoritative and narrowly scoped. A client event, UI click, or keybind alone must never be sufficient evidence for moderation.

## Capability example

```toml
[server]
entry = "server/main.lua"
capabilities = ["chat", "players.kick"]
```

Only request capabilities the resource actually uses. An undeclared privileged API call raises a Lua error.
