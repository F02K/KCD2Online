# Client Lua API

[Back to scripting overview](../README.md)

Client Lua is optional and intentionally small. Use it only when local code must react to or emit custom resource events. Declarative UI and keybinds already work without client Lua.

## Enable client Lua

Add a client section to `resource.toml`:

```toml
[client]
entry = "client/main.lua"
capabilities = []
```

All regular files below `client/` become part of the resource package. Client capabilities must currently remain empty.

## Available API

```lua
events.on(name, callback)
events.emit_server(name, payload)
require(module_name)
print(...)
```

The Lua base, `table`, `string`, and `math` libraries are available. There is no client `server`, `timer`, `ui`, or `input` table, and no file, OS, debug, package, socket, native DLL, game memory, or raw ImGui access.

## Receive a server event

Manifest:

```toml
[[events]]
name = "weather.notice"
direction = "server_to_client"
reliable = true
max_per_second = 2
max_bytes = 2048
```

Client code:

```lua
events.on("weather.notice", function(payload)
    if type(payload) == "table" and type(payload.text) == "string" then
        print(payload.text)
    end
end)
```

The callback receives the decoded JSON payload as one Lua value.

## Send a server event

Manifest:

```toml
[[events]]
name = "client.ready"
direction = "client_to_server"
reliable = true
max_per_second = 1
max_bytes = 512
```

Client code:

```lua
events.emit_server("client.ready", {
    interface_version = 1
})
```

Events emitted while the package is loading are queued locally until the server accepts the world join. The local queue holds at most 256 outgoing and deferred resource events.

## Client modules

`client/format.lua`:

```lua
local M = {}

function M.notice(text)
    return "[Server] " .. tostring(text)
end

return M
```

`client/main.lua`:

```lua
local format = require("format")

events.on("weather.notice", function(payload)
    print(format.notice(payload.text))
end)
```

## Runtime behavior

Each client resource runs in a separate sandbox with a 16 MiB memory limit and a 150,000-instruction budget per entry execution or callback. After three callback errors, that client resource is disabled until resources are loaded again.

Never place secrets or authority in client Lua. Players receive the source, and a custom client can inspect, replace, or skip it. The server must remain correct when client Lua sends false data or does not run at all.
