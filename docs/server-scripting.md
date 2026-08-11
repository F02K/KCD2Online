# Server-side Lua scripting

KCD2Online loads resources separately for each server. A resource is a directory under `resources/` with exactly one `resource.toml` manifest. Lua is statically linked into `KCD2OnlineServer.exe` and `KCD2OnlineKCSEClient.dll`. A dedicated server therefore does not need a separate Lua installation, the game, KCSE, or a complete checkout of the KCD2Online repository.

## Quick start with the server ZIP

1. Create a directory under `resources/`.
2. Add `resource.toml` and at least one entry point under `server/` or `client/`.
3. Restart the server. Resources are deliberately loaded only during startup; hot reload is not supported.
4. Manifest and loading errors are printed to the server console and prevent startup. Output from server-side `print(...)` is prefixed with `[resource]`.

A `server/` directory is sufficient for server-controlled UI and keybinds. Add a `client/` directory only when Lua code actually needs to run on players' computers.

## Directory structure

```text
resources/
  my_script/
    resource.toml
    server/
      main.lua
      another_module.lua
    client/
      main.lua
    shared/
      data.json
```

`server/` and `client/` are optional. A server-only resource declares only `[server]`, and no code is transferred to clients. If `[client]` is present, the server packages only `client/` and files from `shared/` that are explicitly allowlisted. Files under `server/` can never become part of a client package.

## Manifest

```toml
[resource]
id = "my_script"
version = "1.0.0"
api_version = 1
dependencies = []

[server]
entry = "server/main.lua"
capabilities = ["chat", "ui", "input", "players.kick"]

[client]
entry = "client/main.lua"
capabilities = []

[shared]
client_paths = ["shared/data.json"]

[[events]]
name = "action"
direction = "bidirectional" # client_to_server | server_to_client | bidirectional
reliable = true
max_per_second = 10
max_bytes = 4096
```

IDs may contain lowercase letters, digits, `.`, `_`, and `-`. Dependencies are loaded in topological order; missing or cyclic dependencies stop server startup. Entry paths must remain inside their respective directories. Symlinks, `..`, absolute paths, and Lua bytecode are rejected.

## Server API

- `server.on(name, callback)` registers `start`, `player_joined`, `player_left`, `chat`, `player_death`, or `ui` handlers.
- `server.players()` returns a table of `{ id, name, connected, role }` values.
- `server.say(text [, player_id])` sends a system message. Requires `chat`.
- `server.kick(player_id [, reason])` disconnects a player. Requires `players.kick`.
- `events.on(name, callback)` receives a client event allowed by the manifest. Callback: `(player_id, payload)`.
- `events.emit_client(player_id_or_nil, name, payload)` sends to one player; `nil` or `0` broadcasts to everyone. Direction, size, and reliability are defined by the manifest.
- `timer.after(ms, callback)` and `timer.every(ms, callback)` create bounded server timers.
- `ui.show`, `ui.patch`, `ui.close`, and `ui.toast` require `ui`.
- `input.register`, `input.unregister`, and `input.on` require `input`.

Lifecycle callbacks receive these arguments:

| Name | Callback arguments |
| --- | --- |
| `start` | none |
| `player_joined` | `player` |
| `player_left` | `player, reason` |
| `chat` | `player_id, text` |
| `player_death` | `player_id` |
| `ui` | `player_id, document_id, control_id, event, payload` |

A `player` contains `id`, `name`, `connected`, and `role`. IDs and roles are server-side information, but event and UI payloads received from a client must still be treated as untrusted input.

Example using a lifecycle event and timer:

```lua
server.on("player_joined", function(player)
    server.say("Welcome " .. player.name)
    timer.after(5000, function()
        server.say("You have been connected for five seconds.", player.id)
    end)
end)
```

`require("helper")` loads only `server/helper.lua`, or `client/helper.lua` on the client. File system, operating system, network, native DLL, `loadfile`, `dofile`, and dynamic bytecode access are unavailable.

## Client API

Client-side Lua is intended for local behavior, not authority. It can receive declared events with `events.on(name, callback)` and send them with `events.emit_server(name, payload)`. The receive callback gets the JSON payload as a Lua value. Events emitted while loading are queued locally until the server accepts the world join. The server validates the resource ID, event direction, rate, and payload size again. Setting `reliable = false` uses the unreliable transport in both directions and is suitable only for replaceable updates.

Never trust client claims about save data, money, inventory, position, or permissions. Server-side handlers must always validate values against the current authoritative state.

A resource does not need a `client/` directory to use UI or keybinds: the integrated UI and input runtime is already part of the standard KCD2Online client. This is the recommended approach for most server scripts.

## Error and resource limits

Each resource has its own Lua state. Memory, instructions per callback, JSON depth and size, and repeated runtime errors are limited. The limits are configured under `[resources]` in `server.toml`. After too many errors, only the affected resource is disabled. An initial loading error stops the server so that a partially configured server never goes online.

Additional fixed limits are 128 resources, 512 files per client package, 16 MiB per file, 64 MiB per package, 256 MiB across all client packages, and 32 KiB per event or UI JSON payload. Manifest settings can further reduce an event's size and rate.

The included `welcome_ui` and `event_example` resources can be copied, modified, or deleted.
