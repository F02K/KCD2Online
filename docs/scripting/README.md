# KCD2Online server scripting

This documentation covers per-server Lua resources: sandboxed scripts loaded by `KCD2OnlineServer.exe`, optional client scripts downloaded when a player joins, and server-controlled UI rendered by the built-in client.

This API is separate from the local mod-loader API documented under `docs/lua/`. Server resources cannot access the game process, raw ImGui bindings, the operating system, or the network.

## Start here

1. Follow the [quick start](getting-started/quick-start.md) to create a minimal server-only resource.
2. Read [resource structure](getting-started/resource-structure.md) to decide whether you need `server/`, `client/`, or both.
3. Use the [manifest reference](getting-started/manifest-reference.md) to declare capabilities, dependencies, shared files, and network events.
4. Pick the relevant API guide from the table below.
5. Before publishing, read [sandbox and security](operations/sandbox-and-security.md).

## Feature overview

| Goal | Runs where | Client Lua required? | Guide |
| --- | --- | --- | --- |
| React to joins, leaves, chat, and deaths | Server | No | [Lifecycle events](api/lifecycle-events.md) |
| List players, send messages, or kick | Server | No | [Players and chat](api/players-and-chat.md) |
| Run delayed or repeating work | Server | No | [Timers and modules](api/timers-and-modules.md) |
| Display windows, forms, and notifications | Server logic, built-in client renderer | No | [Declarative UI](api/ui.md) |
| Register player-configurable actions | Server logic, built-in client input runtime | No | [Keybinds](api/keybinds.md) |
| Exchange custom JSON messages | Server and client | Yes | [Custom events](api/custom-events.md) |
| Run local Lua behavior | Client | Yes | [Client API](api/client-api.md) |
| Understand downloads and caching | Server and client runtime | Only for downloadable resources | [Delivery and cache](operations/delivery-and-cache.md) |

## Resource model

Each direct child of the server's `resources/` directory that contains `resource.toml` is one resource. Each resource gets an isolated Lua state.

```text
resources/
  my_resource/
    resource.toml
    server/
      main.lua
    client/                 # optional
      main.lua
    shared/                 # optional, explicitly allowlisted files only
```

- Server-only resources are never downloaded.
- A resource with `[client]` packages all files under `client/` plus explicitly allowlisted `shared/` files.
- Files under `server/` are never included in a client package.
- UI and keybinds do not require client Lua; their renderer and input runtime ship with KCD2Online.
- Server state is authoritative. Every value originating from a client is untrusted.

## API at a glance

```lua
server.on("player_joined", function(player)
    server.say("Welcome " .. player.name, player.id)

    ui.show(player.id, "welcome", {
        title = "Welcome",
        widgets = {
            { type = "text", text = "Connected as " .. player.name },
            { type = "button", id = "close", text = "Close" }
        }
    })

    input.register(player.id, "toggle_welcome", "Toggle welcome window", 0x75)
end)

server.on("ui", function(player_id, document_id, control_id, event, payload)
    if document_id == "welcome" and control_id == "close" and event == "click" then
        ui.close(player_id, document_id)
    end
end)
```

## Reference

### Getting started

- [Quick start](getting-started/quick-start.md)
- [Resource structure and server/client separation](getting-started/resource-structure.md)
- [`resource.toml` manifest reference](getting-started/manifest-reference.md)

### API guides

- [Lifecycle events](api/lifecycle-events.md)
- [Custom client/server events](api/custom-events.md)
- [Players and chat](api/players-and-chat.md)
- [Timers and modules](api/timers-and-modules.md)
- [Declarative UI](api/ui.md)
- [Keybinds](api/keybinds.md)
- [Client API](api/client-api.md)

### Operations

- [Resource delivery and cache](operations/delivery-and-cache.md)
- [Sandbox and security](operations/sandbox-and-security.md)
- [Configuration and limits](operations/configuration-and-limits.md)

### Complete examples

- [Server-only welcome UI](examples/welcome-ui.md)
- [Client/server event round trip](examples/client-server-events.md)
- [Modular announcements with timers](examples/modular-announcements.md)

The server ZIP contains this entire documentation tree and the runnable `welcome_ui` and `event_example` resources. Server operators do not need a source checkout, KCSE installation, SDK, or separate Lua runtime.
