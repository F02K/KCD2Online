# Sandbox and security

[Back to scripting overview](../README.md)

Every resource runs in an isolated Lua state. The sandbox reduces accidental and malicious impact, but it does not make data from players trustworthy.

## Available environment

Both server and client sandboxes provide:

- Lua base functions needed for ordinary scripts;
- `table`, `string`, and `math`;
- resource-prefixed `print(...)`;
- a restricted, resource-local `require(...)`;
- the side-specific KCD2Online API documented in this section.

They do not provide:

- `io`, `os`, `debug`, `package`, or socket libraries;
- arbitrary file-system access;
- environment variables or process execution;
- native DLL or C module loading;
- `dofile`, `loadfile`, `load`, `loadstring`, or precompiled bytecode;
- raw game-memory, KCSE, or ImGui access;
- modules from another resource or from the opposite server/client side.

## Trust boundaries

```text
server Lua and server state     authoritative
client Lua                     untrusted
custom event payloads          untrusted
UI interaction payloads        untrusted
keybind events                 untrusted
display names and chat text    untrusted text
package SHA-256 hashes         integrity check, not client authority
```

A modified client can skip its Lua entry point, send events without running your UI, forge control IDs and values, claim a key was pressed, replay application-level intentions, or inspect all downloaded source.

## Validate every action

Suppose a shop button sends a requested item and amount. The safe handler derives every consequential value from server state:

```lua
server.on("ui", function(player_id, document_id, control_id, event, payload)
    if document_id ~= "shop" or control_id ~= "buy" or event ~= "click" then
        return
    end

    local item_id = type(payload.item_id) == "string" and payload.item_id or nil
    local amount = tonumber(payload.amount)
    if not item_id or not amount or amount < 1 or amount > 10 then
        return
    end

    -- The actual implementation must now check authoritative server state:
    -- player existence, shop availability, distance, stock, price, balance,
    -- ownership, permissions, cooldown, and transaction duplication.
end)
```

The UI shown earlier, its revision, the default keybind, and client Lua checks are usability features—not authorization checks.

## Secrets

Never place passwords, API tokens, private URLs, signing keys, moderation rules that must remain private, or anti-cheat detection logic under `client/` or an allowlisted `shared/` path. Downloadable source is public to the connecting player.

The sandbox intentionally has no outbound network API. If a future resource needs external integration, add a narrowly scoped server-side capability and validation layer to KCD2Online instead of exposing general sockets to Lua.

## Denial-of-service controls

The runtime bounds resource memory, Lua instructions per execution, callback errors, JSON size/depth/complexity, resource counts, package sizes, UI state, keybind counts, outgoing client event queues, and client-to-server event rates.

These controls limit impact; scripts should still avoid unbounded tables, permanent per-player state, high-frequency timers, large broadcasts, and deeply nested payloads.

## Failure isolation

- A manifest, dependency, path, package, or initial entry-point error prevents startup or join rather than accepting a partial configuration.
- A runtime callback error is logged against its resource.
- Reaching the configured error limit disables only that server resource.
- A client resource is disabled locally after repeated callback errors.

Test both valid and hostile payload shapes before publishing a resource.
