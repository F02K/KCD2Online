# Custom client/server events

[Back to scripting overview](../README.md)

Custom events are JSON messages between the server Lua state and the optional client Lua state of the same resource. Every event must be declared in `resource.toml`.

## Declare event direction

```toml
[[events]]
name = "profile.request"
direction = "client_to_server"
reliable = true
max_per_second = 2
max_bytes = 1024

[[events]]
name = "profile.snapshot"
direction = "server_to_client"
reliable = true
max_per_second = 10
max_bytes = 8192
```

Use the narrowest direction possible. Use `bidirectional` only when the same event name genuinely needs to travel both ways.

## Client to server

Client entry point:

```lua
events.emit_server("profile.request", {
    request_id = "menu-open"
})
```

Server entry point:

```lua
events.on("profile.request", function(player_id, payload)
    if type(payload) ~= "table" or payload.request_id ~= "menu-open" then
        return
    end

    -- Build this from authoritative server state, not client claims.
    events.emit_client(player_id, "profile.snapshot", {
        display_name = "Verified player",
        reputation = 12
    })
end)
```

Client-to-server rate limiting is applied per player, resource, and event according to `max_per_second`. Oversized, malformed, undeclared, or wrong-direction messages are rejected.

## Server to client

Client entry point:

```lua
events.on("profile.snapshot", function(payload)
    print("Reputation:", payload.reputation)
end)
```

Send to one player:

```lua
events.emit_client(player_id, "profile.snapshot", payload)
```

Broadcast to every connected player by passing `nil` or `0`:

```lua
events.emit_client(nil, "profile.snapshot", payload)
```

## Payload types

Lua values are converted to JSON. Supported values are:

- `nil` as JSON `null`;
- booleans;
- finite JSON-compatible numbers;
- strings;
- tables with consecutive numeric keys starting at `1` as arrays;
- tables with string keys as objects.

Functions, userdata, threads, mixed/non-consecutive table keys, empty object keys, and overly complex values are rejected. Payloads are limited by the event's `max_bytes`, with an absolute maximum of 32 KiB, a nesting limit of 16, and a complexity limit of 2,048 values.

## Reliable versus unreliable

`reliable = true` preserves delivery and ordering through the reliable transport. Use it for purchases, menu actions, acknowledgments, and state transitions.

`reliable = false` uses the unreliable transport in both directions. Use it only for replaceable data where a later update makes a lost update irrelevant. Never use an unreliable event for a transaction that must occur exactly once.

## Security checklist

For every client-to-server event:

1. Check that the payload has the expected types and range.
2. Resolve the player from the callback's `player_id`; never accept a player ID from the payload as authority.
3. Recheck permissions, ownership, distance, current state, cooldown, price, and inventory on the server.
4. Make important operations idempotent or protect them with server-side state.
5. Return only data the target player is allowed to receive.
