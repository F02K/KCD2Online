# Quick start

[Back to scripting overview](../README.md)

This example creates a server-only resource that greets each joining player. It requires no client script and causes no resource download.

## 1. Create the directories

Create this layout next to `KCD2OnlineServer.exe`:

```text
resources/
  greeting/
    resource.toml
    server/
      main.lua
```

## 2. Create the manifest

Save this as `resources/greeting/resource.toml`:

```toml
[resource]
id = "greeting"
version = "1.0.0"
api_version = 1
dependencies = []

[server]
entry = "server/main.lua"
capabilities = ["chat"]
```

The `chat` capability is required because this script calls `server.say`.

## 3. Create the script

Save this as `resources/greeting/server/main.lua`:

```lua
server.on("start", function()
    print("Greeting resource started")
end)

server.on("player_joined", function(player)
    server.say("Welcome to the server, " .. player.name .. "!", player.id)
end)
```

## 4. Start the server

Start or restart the dedicated server. Resources are loaded during startup; hot reload is not supported. You should see the resource's `print` output in the server console with a resource prefix.

If the manifest is invalid or the entry script fails during initial execution, server startup stops. Runtime callback errors are logged, and only the affected resource is disabled after it reaches the configured error limit.

## Add UI without client Lua

Add `ui` to the capability list:

```toml
capabilities = ["chat", "ui"]
```

Then add this inside the `player_joined` callback:

```lua
ui.toast(player.id, "welcome", {
    text = "Welcome to the server!"
})
```

No `[client]` section is needed. The normal KCD2Online client already contains the declarative UI renderer.

## Next steps

- Learn when client Lua is necessary in [resource structure](resource-structure.md).
- See every manifest field in the [manifest reference](manifest-reference.md).
- Build a window using the [UI guide](../api/ui.md).
