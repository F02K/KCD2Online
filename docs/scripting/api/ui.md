# Declarative UI

[Back to scripting overview](../README.md)

Server resources describe UI as Lua tables that are serialized to JSON and rendered by the client's built-in ImGui layer. Server Lua never receives a raw ImGui object and never runs on the render thread. A resource needs the `ui` capability but does not need client Lua.

## API

```lua
ui.show(player_id, document_id, document)
ui.patch(player_id, document_id, merge_patch)
ui.close(player_id, document_id)
ui.toast(player_id, toast_id, toast)
```

`player_id` must identify one positive player. UI calls do not currently broadcast; enumerate `server.players()` to show a document to several players.

Document, toast, control, and action IDs should be stable identifiers. Interactive control IDs must start with a letter, be at most 64 characters, and contain only letters, digits, `_`, `.`, and `-`.

## Document properties

```lua
ui.show(player_id, "character_panel", {
    title = "Character",
    visible = true,
    position = { 80, 120 },
    size = { 440, 320 },
    movable = true,
    widgets = {
        { type = "text", text = "Overview" }
    }
})
```

| Field | Default | Meaning |
| --- | --- | --- |
| `title` | `document_id` | Visible window title |
| `visible` | `true` | Whether the window is rendered |
| `position` | ImGui default | Initial `{ x, y }` position |
| `size` | ImGui default | Initial `{ width, height }` |
| `movable` | `true` | Whether the player may move the window |
| `widgets` | `[]` | Ordered array of widget specifications |

Position and size are applied when the window appears. Unknown document fields are ignored.

## Widget reference

All widgets accept `type`. They may also set `same_line = true` to appear after the previous widget.

| Type | Useful fields | Emitted event |
| --- | --- | --- |
| `text` | `text` | none |
| `separator` | `text` | none |
| `spacer` | `height` (default `8`) | none |
| `button` | `id`, `text` | `click`, `{}` |
| `checkbox` | `id`, `text`, `value` | `change`, `{ value = boolean }` |
| `slider` | `id`, `text`, `value`, `min`, `max` | `change`, `{ value = number }` |
| `input` | `id`, `text`, `value` | `submit` on Enter, `{ value = string }` |
| `progress` | `text`, `value` from `0` to `1` | none |

`label` can be used as a fallback when `text` is absent. Unknown widget types are skipped. Input fields currently hold up to 1,023 text bytes locally.

## Complete form

```lua
ui.show(player_id, "shop", {
    title = "Merchant",
    size = { 460, 340 },
    widgets = {
        { type = "text", text = "Available goods" },
        { type = "separator", text = "Order" },
        { type = "checkbox", id = "bulk", text = "Bulk purchase", value = false },
        { type = "slider", id = "amount", text = "Amount", value = 1, min = 1, max = 10 },
        { type = "input", id = "note", text = "Note", value = "" },
        { type = "button", id = "buy", text = "Buy" },
        { type = "button", id = "close", text = "Close", same_line = true },
        { type = "progress", text = "Reputation", value = 0.65 }
    }
})
```

## Handle interactions

Register the lifecycle handler once in the server entry point:

```lua
server.on("ui", function(player_id, document_id, control_id, event, payload)
    if document_id ~= "shop" then
        return
    end

    if control_id == "buy" and event == "click" then
        -- Recalculate price, stock, ownership, range, and permissions here.
        server.say("Order received.", player_id)
    elseif control_id == "amount" and event == "change" then
        local amount = tonumber(payload.value)
        if amount and amount >= 1 and amount <= 10 then
            print("Requested amount", player_id, amount)
        end
    elseif control_id == "note" and event == "submit" then
        local note = type(payload.value) == "string" and payload.value or ""
        print("Submitted note", player_id, note)
    elseif control_id == "close" and event == "click" then
        ui.close(player_id, document_id)
    end
end)
```

The client also sends a document revision, but it is not proof that the event came from the displayed UI. A modified client can forge every field.

## Update a document

`ui.show` creates a document or replaces its complete specification. `ui.patch` applies JSON Merge Patch semantics:

```lua
ui.patch(player_id, "status", {
    title = "Updated status",
    visible = true
})
```

Object fields merge recursively. A JSON null removes a field. Lua `nil` cannot remain as a table value, so replacing the full document with `ui.show` is often clearer when fields must be removed. Arrays such as `widgets` are always replaced as a whole:

```lua
ui.patch(player_id, "status", {
    widgets = {
        { type = "text", text = "The new complete widget list" }
    }
})
```

Close unused documents:

```lua
ui.close(player_id, "status")
```

## Toasts

```lua
ui.toast(player_id, "purchase_complete", {
    text = "Purchase completed."
})
```

Toasts appear in the lower-right corner for five seconds. The client retains at most eight toast entries.

## Client limits

A client holds at most 64 documents and 256 KiB of total declarative UI state. Close documents that are no longer needed and keep widget text and update frequency reasonable.
