# Configuration and limits

[Back to scripting overview](../README.md)

Server resource settings live in `server.toml`:

```toml
[resources]
enabled = true
directory = "resources"
memory_limit_mb = 32
instruction_limit = 250000
error_limit = 3
```

Relative resource directories are resolved next to the server configuration file.

## Server settings

| Setting | Default | Allowed range | Meaning |
| --- | ---: | ---: | --- |
| `enabled` | `true` | Boolean | Enables resource discovery and server Lua |
| `directory` | `resources` | Non-empty path | Directory containing resource subdirectories |
| `memory_limit_mb` | `32` | `4`–`256` | Memory limit for each server resource Lua state |
| `instruction_limit` | `250000` | `10000`–`10000000` | Lua instruction budget for each entry execution or callback |
| `error_limit` | `3` | `1`–`100` | Runtime callback errors before a server resource is disabled |

Increasing a limit can reduce isolation for every resource. Prefer fixing a script's allocation or loop behavior before raising global limits.

## Fixed resource and package limits

| Limit | Value |
| --- | ---: |
| Resources per server | 128 |
| Files per client package | 512 |
| Size per client file | 16 MiB |
| Size per client package | 64 MiB |
| Combined client packages | 256 MiB |
| Transfer chunk | 48 KiB |
| Event or UI JSON maximum | 32 KiB |
| JSON nesting | 16 levels |
| JSON complexity | 2,048 values |
| JSON string | 32 KiB |
| JSON object key | 128 bytes |

An event's `max_bytes` can lower the payload limit to between 1 and 32,768 bytes. Its `max_per_second` can be between 1 and 100 and limits incoming messages per player, resource, and event.

## Timer limits

| Limit | Value |
| --- | ---: |
| Minimum delay/interval | 1 ms |
| Maximum delay/interval | 86,400,000 ms (24 hours) |

There is currently no cancellation API. Repeating timers live until their resource stops or is disabled.

## Client runtime limits

| Limit | Value |
| --- | ---: |
| Memory per client resource | 16 MiB |
| Instructions per client entry/callback | 150,000 |
| Callback errors before local disable | 3 |
| Queued/deferred outgoing client events | 256 |
| UI documents | 64 |
| Dynamic keybinds | 128 |
| Declarative UI state | 256 KiB |
| Retained toast entries | 8 |

## Identifier limits

Resource IDs use lowercase letters, digits, `.`, `_`, and `-`, start with an alphanumeric character, and are at most 64 characters.

Event, document, control, and action IDs start with a letter, may additionally use uppercase letters, digits, `.`, `_`, and `-`, and are at most 64 characters. Prefer lowercase dotted names for custom events and lowercase snake-case names for controls.

## Operational behavior

- Resources load only during server startup.
- Missing or cyclic dependencies stop startup.
- Initial Lua load errors stop startup.
- There is no hot reload.
- A server-only change does not require a client download.
- A changed client package is downloaded on the next join.
