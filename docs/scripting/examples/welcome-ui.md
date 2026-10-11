# Example: server-only welcome UI

[Back to scripting overview](../README.md)

This resource displays a welcome window, handles its buttons, and registers an F6 action that toggles the window. It uses no client Lua and is not downloaded as a script package.

The same example is included in the server ZIP under `resources/welcome_ui/`.

## Layout

```text
resources/
  welcome_ui/
    resource.toml
    server/
      main.lua
```

## `resource.toml`

```toml
[resource]
id = "welcome_ui"
version = "1.0.0"
api_version = 1
dependencies = []

[server]
entry = "server/main.lua"
capabilities = ["chat", "ui", "input"]
```

## `server/main.lua`

```lua
local open = {}

local function find_player(player_id)
    for _, player in ipairs(server.players()) do
        if player.id == player_id then
            return player
        end
    end
    return nil
end

local function show_dashboard(player)
    open[player.id] = true

    ui.show(player.id, "welcome", {
        title = "KCD2Online Server",
        size = { 440, 260 },
        widgets = {
            { type = "text", text = "Welcome, " .. player.name .. "!" },
            { type = "separator", text = "Server Resource UI" },
            { type = "text", text = "This window is controlled by server Lua." },
            { type = "progress", text = "Example", value = 0.72 },
            { type = "button", id = "hello", text = "Say hello" },
            { type = "button", id = "close", text = "Close", same_line = true }
        }
    })
end

server.on("player_joined", function(player)
    input.register(player.id, "toggle_welcome", "Welcome window", 0x75) -- F6
    show_dashboard(player)
end)

server.on("player_left", function(player, reason)
    open[player.id] = nil
end)

server.on("ui", function(player_id, document_id, control_id, event, payload)
    if document_id ~= "welcome" or event ~= "click" then
        return
    end

    if control_id == "hello" then
        server.say("Hello from Lua!", player_id)
    elseif control_id == "close" then
        open[player_id] = false
        ui.close(player_id, "welcome")
    end
end)

input.on("toggle_welcome", function(player_id, payload)
    if payload.pressed ~= true then
        return
    end

    if open[player_id] then
        open[player_id] = false
        ui.close(player_id, "welcome")
        return
    end

    local player = find_player(player_id)
    if player then
        show_dashboard(player)
    end
end)
```

## What this demonstrates

- Capability declaration for chat, UI, and input.
- Per-player Lua state indexed by numeric player ID.
- UI events filtered by document, control, and event.
- A player-configurable keybind without client Lua.
- Cleanup when a player leaves.

For production actions, perform authorization and current-state checks inside the callback. The example's greeting is intentionally harmless.
