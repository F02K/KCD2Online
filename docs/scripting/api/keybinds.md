# Keybinds

[Back to scripting overview](../README.md)

Server resources can register named actions on individual clients. The built-in client runtime detects the key and sends the action back to server Lua. This requires the `input` capability but not client Lua.

## Register an action

```lua
input.register(player_id, "toggle_shop", "Toggle shop window", 0x75) -- F6
```

Signature:

```lua
input.register(player_id, action_id, label, default_virtual_key)
```

- `player_id` must be positive.
- `action_id` follows the normal event-name rules and is at most 64 characters.
- `label` must contain 1 through 384 bytes.
- `default_virtual_key` is a Windows virtual-key code from 1 through 255.
- F8 (`0x77`) is reserved for the keybind editor and cannot be registered.

Common defaults:

| Key | Code | Key | Code |
| --- | --- | --- | --- |
| `0` through `9` | `0x30` through `0x39` | `A` through `Z` | `0x41` through `0x5A` |
| F1 | `0x70` | F5 | `0x74` |
| F2 | `0x71` | F6 | `0x75` |
| F3 | `0x72` | F7 | `0x76` |
| F4 | `0x73` | F9 | `0x78` |

## Handle an action

Register a callback by action ID:

```lua
input.on("toggle_shop", function(player_id, payload)
    if payload.pressed == true then
        server.say("Shop key pressed.", player_id)
    end
end)
```

The current payload is `{ pressed = true }`. Register the callback once at entry-point load time; call `input.register` for every player who should receive the binding.

## Join and leave pattern

```lua
server.on("player_joined", function(player)
    input.register(player.id, "toggle_help", "Toggle server help", 0x70) -- F1
end)

server.on("player_left", function(player, reason)
    -- Clean up any Lua state associated with player.id here.
end)

input.on("toggle_help", function(player_id, payload)
    ui.show(player_id, "help", {
        title = "Server help",
        widgets = {
            { type = "text", text = "Welcome to the server." },
            { type = "button", id = "close", text = "Close" }
        }
    })
end)
```

## Remove an action

```lua
input.unregister(player_id, "toggle_help")
```

Remove temporary actions when a mode, job, activity, or UI is no longer available.

## Player overrides

Players press F8 to open the local server keybind editor. Overrides are stored per server, resource, and action under:

```text
%LOCALAPPDATA%\KCD2Online\resource-keybinds.json
```

The server supplies a default but cannot force the player's final local choice. Key events are suppressed while chat, the keybind editor, or a UI text input is active. A client holds at most 128 dynamic bindings.

## Security boundary

A key event is a request, not evidence. A modified client can emit the action without pressing a key. Before performing an authoritative operation, validate the player's state, permissions, cooldown, distance, inventory, and any other relevant rule on the server.
