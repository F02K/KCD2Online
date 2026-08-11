# Declarative server UI and keybinds

Server UI is represented as a validated JSON document passed to the built-in ImGui layer. Lua receives no direct ImGui pointer and never executes code on the render thread. This keeps rendering, input, and networking isolated from one another.

The complete function signatures are:

```lua
ui.show(player_id, document_id, document)
ui.patch(player_id, document_id, merge_patch)
ui.close(player_id, document_id)
ui.toast(player_id, toast_id, { text = "Message" })
```

You can obtain `player_id` from `player_joined` or `server.players()`, for example. Document, control, toast, and action IDs should be stable, short ASCII identifiers such as `shop` or `buy_item`.

## Showing a window

```lua
ui.show(player_id, "shop", {
    title = "Merchant",
    position = { 80, 120 },
    size = { 440, 320 },
    movable = true,
    widgets = {
        { type = "text", text = "Available goods" },
        { type = "separator", text = "Selection" },
        { type = "checkbox", id = "bulk", text = "Bulk purchase", value = false },
        { type = "slider", id = "amount", text = "Amount", value = 1, min = 1, max = 10 },
        { type = "input", id = "note", text = "Note", value = "", },
        { type = "button", id = "buy", text = "Buy" },
        { type = "progress", text = "Reputation", value = 0.65 },
        { type = "spacer", height = 8 }
    }
})
```

Supported widget types are `text`, `separator`, `spacer`, `button`, `checkbox`, `slider`, `input`, and `progress`. Set `same_line = true` to place an element after the previous one. Unknown fields are ignored; unknown widget types are not rendered.

`ui.patch(player_id, document_id, patch)` uses JSON Merge Patch semantics. `ui.close(player_id, document_id)` closes the window. `ui.toast(player_id, toast_id, { text = "Message" })` displays a notification in the lower-right corner for five seconds.

## UI events

```lua
server.on("ui", function(player_id, document_id, control_id, event, payload)
    if document_id == "shop" and control_id == "buy" and event == "click" then
        -- Always validate price, stock, and permissions again on the server.
    end
end)
```

Buttons emit `click`; checkboxes and sliders emit `change`; text inputs emit `submit` when Enter is pressed. The payload contains a value such as `{ value = true }`. The document revision is also transmitted, but it is not proof of authority.

`ui.patch` follows JSON Merge Patch: object fields are replaced or merged recursively, and `nil`/JSON `null` removes a field. Arrays such as `widgets` are replaced as a whole.

## Keybinds

```lua
input.register(player_id, "open_shop", "Open shop", 0x75) -- F6

input.on("open_shop", function(player_id, payload)
    -- Open or close the UI
end)
```

`default_virtual_key` uses Windows virtual-key codes from 1 through 255; F8 (`0x77`) is reserved for the editor. Use `input.unregister(player_id, action_id)` to remove an action. Players can press F8 to open the local server keybind editor and select another key. Bindings are stored per server and resource in `%LOCALAPPDATA%\KCD2Online\resource-keybinds.json`. Key presses are not emitted while chat or a text input is active.

An action becomes available on the target client only after `input.register`. The callback registered by `input.on(action_id, callback)` receives `(player_id, payload)`; the current payload is `{ pressed = true }`. Authoritative game actions must never depend solely on the client's claim that a key was pressed.

To protect the client, the runtime holds at most 64 UI documents, 128 dynamic bindings, and 256 KiB of declarative UI state at the same time. Unused windows and actions should still always be removed with `ui.close` and `input.unregister`, respectively.

## Security model

UI is never trusted. A modified client can forge any UI or keybind event. The server must therefore validate distance, state, ownership, price, cooldown, and permissions again for every action. Hashes protect a normal client from corrupt or locally modified cache files; they do not make a custom, deliberately modified client trustworthy.
