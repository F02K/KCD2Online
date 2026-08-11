# `resource.toml` manifest reference

[Back to scripting overview](../README.md)

The manifest defines resource identity, entry points, capabilities, dependencies, downloadable shared paths, and custom network events.

## Complete example

```toml
[resource]
id = "example.shop"
version = "1.2.0"
api_version = 1
dependencies = ["example.core"]

[server]
entry = "server/main.lua"
capabilities = ["chat", "ui", "input", "players.kick"]

[client]
entry = "client/main.lua"
capabilities = []

[shared]
client_paths = ["shared/public-data.json", "shared/icons"]

[[events]]
name = "shop.request"
direction = "client_to_server"
reliable = true
max_per_second = 4
max_bytes = 2048

[[events]]
name = "shop.updated"
direction = "server_to_client"
reliable = true
max_per_second = 10
max_bytes = 8192
```

## `[resource]`

| Field | Required | Meaning |
| --- | --- | --- |
| `id` | Yes | Unique ID, at most 64 characters. Lowercase letters, digits, `.`, `_`, and `-`; first character must be alphanumeric. |
| `version` | Yes | Version string, at most 32 characters. Letters, digits, `.`, `-`, and `+` are accepted. |
| `api_version` | No | Defaults to the current value. Currently only `1` is accepted. |
| `dependencies` | No | Resource IDs that must load first. Defaults to `[]`. |

Missing dependencies, self-dependencies, duplicates, and dependency cycles stop server startup. Dependencies control load order; they do not expose another resource's Lua state or modules.

## `[server]`

| Field | Required | Meaning |
| --- | --- | --- |
| `entry` | Yes when the section exists | Lua entry point below `server/`. |
| `capabilities` | No | Explicit permissions used by privileged server APIs. Defaults to `[]`. |

Supported capabilities:

| Capability | Enables |
| --- | --- |
| `chat` | `server.say` |
| `ui` | `ui.show`, `ui.patch`, `ui.close`, `ui.toast`, and `server.on("ui", ...)` |
| `input` | `input.register`, `input.unregister`, and `input.on` |
| `players.kick` | `server.kick` |

`server.players`, lifecycle callbacks, timers, modules, and declared custom events need no capability.

## `[client]`

| Field | Required | Meaning |
| --- | --- | --- |
| `entry` | Yes when the section exists | Lua entry point below `client/`. |
| `capabilities` | No | Must currently be empty; client capabilities are not implemented. |

Declaring `[client]` creates a downloadable package. Omit the entire section for a server-only resource.

## `[shared]`

`client_paths` is an array of individual files or directories below `shared/`. Listed directories are included recursively. These files become part of the client package, but the Lua sandbox still has no general file-system API.

## `[[events]]`

Each custom event needs a unique name. Names are at most 64 characters, start with a letter, and may contain letters, digits, `_`, `.`, and `-`.

| Field | Default | Valid values |
| --- | --- | --- |
| `name` | Required | Valid event name |
| `direction` | `bidirectional` | `client_to_server`, `server_to_client`, `bidirectional` |
| `reliable` | `true` | Boolean |
| `max_per_second` | `10` | `1` through `100` |
| `max_bytes` | `4096` | `1` through `32768` |

Direction is enforced on both API registration and transmission. Client-to-server events are rate-limited per player, resource, and event. Always validate payload contents in the server handler.
