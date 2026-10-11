# Client Lua API

[Back to scripting overview](../README.md)

Client Lua owns local presentation and interaction policy for client-enabled
resources. Native C++ services provide rendering primitives, input delivery,
audio/Opus, transport, persistence, and game integration.

## Enable client Lua

Add a client section to `resource.toml`:

```toml
[client]
entry = "client/main.lua"
capabilities = ["input", "ui"]
```

All regular files below `client/` become part of the resource package.
Privileged local services require an explicit client capability. Native feature
providers also declare the corresponding `feature.*` capability documented in
[built-in feature providers](builtin-features.md).

## Available API

```lua
events.on(name, callback)
events.emit_server(name, payload)
features.enable(name)
input.bind(action, label, default_virtual_key [, emit_release])
input.on(action, callback)
ui.show(document, specification)
ui.patch(document, merge_patch)
ui.close(document)
ui.toast(toast_id, specification)
ui.on(callback)
voice.set_transmitting(active [, range])
require(module_name)
print(...)
```

The Lua base, `table`, `string`, and `math` libraries are available. There is no
client `server` table and no file, OS, debug, package, socket, native DLL, game
memory, or raw ImGui access.

## Local input

`input.bind` requires the client `input` capability. The binding is visible in
the client's F8 keybind editor and its override remains client-local. A normal
binding emits once when the key is released. Pass `true` as the fourth argument
for press and release events, which is appropriate for push-to-talk:

```lua
input.bind("talk", "Voice: push to talk", 0x56, true) -- V
input.on("talk", function(event)
    print(event.pressed, event.control, event.shift, event.alt)
end)
```

Callbacks execute in client Lua without a server round trip.

## Local UI

The client `ui` capability enables local documents. Their schema uses the same
widgets as [declarative UI](ui.md), but calls omit `player_id`. `ui.on` receives
`document_id, control_id, event, payload`.

`kind = "voice_indicator"` selects the native low-level voice meter primitive.
Its placement, dimensions, and normal/whisper/shout colors still come from the
Lua document. This keeps per-frame audio state out of Lua while leaving the
product presentation in the resource.

## Voice service

`voice.set_transmitting(active, range)` requires the client `voice` capability
and an enabled `voice_chat` provider. `range` is `"whisper"`, `"normal"`, or
`"shout"`. C++ performs capture, processing, Opus encoding, transport, spatial
playback, and device access; Lua decides when and how that service is used.

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

Each client resource runs in a separate sandbox with a 16 MiB memory limit and a 150,000-instruction budget per entry execution or callback. After three callback errors, that client resource is disabled until resources are loaded again. Local input callbacks are queued onto the client's networking/resource thread instead of executing inside the Windows message handler.

Never place secrets or authority in client Lua. Players receive the source, and a custom client can inspect, replace, or skip it. The server must remain correct when client Lua sends false data or does not run at all.
