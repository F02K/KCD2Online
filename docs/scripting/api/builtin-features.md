# Built-in feature providers

[Back to scripting overview](../README.md)

KCD2Online ships native framework components for latency-sensitive or deeply
integrated features. They do not become server features on their own. A Lua
resource must claim the corresponding capability and call
`features.enable(name)`.

This keeps the native renderer, audio codec, transport, persistence, and
validation code reusable while allowing a server operator to install, remove,
or replace the product feature that uses it.

## Available providers

| Feature name | Capability | Side | Native framework supplied |
| --- | --- | --- | --- |
| `text_chat` | `feature.text_chat` | Server and client | Existing chat HUD and validated chat transport |
| `voice_chat` | `feature.voice_chat` | Server and client | Capture, Opus, spatial transport, playback, and voice HUD primitive |
| `player_hub` | `feature.player_hub` | Client | Existing F2 player hub |
| `social_menu` | `feature.social_menu` | Client | Existing F3 social menu |
| `staff_menu` | `feature.staff_menu` | Client | Existing F7 staff menu |
| `properties` | `feature.properties` | Server and client | Property persistence, authorization, protocol, and management panel |

A feature capability may have only one provider on each side. Duplicate
providers stop resource loading instead of making activation order-dependent.

## Activating a feature

Manifest:

```toml
[server]
entry = "server/main.lua"
capabilities = ["feature.text_chat"]

[client]
entry = "client/main.lua"
capabilities = ["feature.text_chat"]
```

Server and client entry point:

```lua
features.enable("text_chat")
```

Calling `features.enable` without the matching side-specific capability is a
script error. Merely declaring the capability does not activate presentation or
server handling.

## Bundled defaults

The server package contains separate providers:

- `default_text_chat`
- `default_voice_chat`
- `default_player_hub`
- `default_social_menu`
- `default_staff_menu`
- `default_properties`

Together they preserve the existing KCD2Online behavior and presentation. A
server operator can remove an individual directory before startup to disable
that feature. Removing text chat or voice also causes the server to ignore the
corresponding client traffic. Removing the property provider disables property
authorization and its management data/UI while retaining ordinary world-object
synchronization.

The provider interface is deliberately separate from generic declarative UI
and custom events. A replacement resource may activate the native provider for
the unchanged KCD2Online experience, or omit it and implement a different
presentation using `ui`, `input`, and declared events.

The bundled voice resource demonstrates the intended split: Lua declares the
default V binding, push-to-talk behavior, modifier-based range selection, and
voice-indicator appearance. The native `voice` service has no hard-coded talk
key; it only executes the requested capture/Opus/playback mechanics.
