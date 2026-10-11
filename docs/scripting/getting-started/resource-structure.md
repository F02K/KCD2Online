# Resource structure and separation

[Back to scripting overview](../README.md)

Every resource is a direct child of the configured resource directory and contains one `resource.toml` manifest. The default directory is `resources/` beside the server configuration.

## Server-only resource

```text
resources/chat_rules/
  resource.toml
  server/
    main.lua
    filters.lua
```

Use this layout for lifecycle hooks, authoritative rules, chat, players, timers, declarative UI, and keybinds. Because the manifest has no `[client]` section, no package is advertised or downloaded.

## Client/server resource

```text
resources/local_effects/
  resource.toml
  server/
    main.lua
  client/
    main.lua
    effects.lua
  shared/
    public-data.json
```

Use client Lua only for behavior that must execute locally and cannot be expressed through the built-in UI/input API. Server and client scripts communicate using events declared in the manifest.

When `[client]` is present:

- every regular file below `client/` is packaged;
- only paths listed in `[shared].client_paths` are additionally packaged;
- `server/` is never packaged;
- the client validates package and individual file hashes before execution.

The current sandbox does not expose arbitrary file-system reads. `require` loads Lua modules from the side on which it executes; it does not load JSON or arbitrary shared files.

## Entry points and modules

Entry points must stay under the matching directory:

```toml
[server]
entry = "server/main.lua"

[client]
entry = "client/main.lua"
```

Server code can load `server/helpers.lua` with:

```lua
local helpers = require("helpers")
```

Client code resolves the same call to `client/helpers.lua`. Dotted module names map to directories: `require("lib.format")` loads `server/lib/format.lua` or `client/lib/format.lua`.

Modules are cached. A module should return a table or function:

```lua
local M = {}

function M.player_label(player)
    return player.name .. " (#" .. player.id .. ")"
end

return M
```

## Path rules

Resource paths must use forward slashes and remain relative to the resource root. The loader rejects:

- absolute paths;
- `.` or `..` path segments;
- backslashes and drive-letter paths;
- symbolic links anywhere in a resource;
- precompiled Lua bytecode.

Keep private rules, credentials, signing material, economy calculations, and permission decisions under `server/`. Client code is downloadable and can always be inspected or replaced by the player.
