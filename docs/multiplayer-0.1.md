# KCD2Online 0.1 multiplayer status

## Implemented foundation

The Version 0.1 foundation is implemented as four CMake targets:

- `KCD2OnlineProtocol`: Protobuf envelope, shared types, UTF-8 and 64 KiB limits.
- `KCD2OnlineNetworking`: GameNetworkingSockets 1.5.1 Direct-IP transport.
- `KCD2OnlineServerCore`: transport-independent authoritative session logic.
- `KCD2OnlineServer`: standalone Windows-x64 console server.

The existing `KCD2Online` DLL contains the matching client, a dedicated network thread, bounded
game-thread command queue, reconnect scheduling, snapshot interpolation/extrapolation, local
movement correction, ImGui page, and game-console commands.

The transport uses reliable messages for handshake, lifecycle, chat, corrections, ping/pong, and
shutdown. Client transforms and world snapshots use unreliable/no-delay delivery. The server ticks
at 30 Hz and publishes snapshots at 20 Hz by default.

## Server operation

Copy `server.toml.example` next to `KCD2OnlineServer.exe` as `server.toml`. The only required value
without a useful default is `server.level_id`; it must equal the level ID shown by the client.

```toml
[server]
bind_address = "0.0.0.0"
port = 27020
name = "KCD2Online Server"
password = ""
max_players = 8
level_id = "3"
required_content_hash = ""

[environment]
# Absolute player count. With fewer players online, the requirement is capped
# at the number of connected, living players.
sleeping_players_required = 2
sleep_wake_hour = 6.0
```

While connected, fast travel and the standalone wait action are disabled.
Sleeping in a bed advances the shared server clock once the configured number
of players is asleep. Death keeps the game's initial death prompt, then replaces
save selection with a server-authoritative respawn at the world spawn.

Start the server:

```powershell
.\KCD2OnlineServer.exe .\server.toml
```

The port is UDP. LAN clients connect to the host's LAN IPv4 address; internet hosts must allow the
port in Windows Firewall and configure UDP port forwarding. The server requires neither KCD2 nor
Steam.

Commands:

- `status`
- `players`
- `kick <player_id> [reason]`
- `say <text>`
- `stop`
- `help`

An unexpected connection loss freezes the player session for 30 seconds. A cryptographically
random, memory-only resume token restores the same `PlayerId`. An intentional disconnect removes
the player immediately.

## Client operation

No save is required. From the title screen the authenticated server bootstrap invokes KCD2's
native debug-New-Game helper for the registered target level. The client preserves the expected
runtime-epoch changes and waits for `DataLoaded`, the target `wh_sys_BaseLevelId`, the local actor,
and the native capability probe before applying authoritative state. Production level IDs are
`2` (`trosecko`), `3` (`kutnohorsko`), and `4` (`klaster`).

Use the **KCD2Online Multiplayer** ImGui page for address, name, optional password, status, player
list, diagnostics, and global chat. Address and name are persisted. Password and resume token are
not logged; the password input is cleared after connecting.

Console commands:

- `mp_connect <host:port> [name]`
- `mp_disconnect`
- `mp_status`
- `mp_say <text>`

The network thread never calls game APIs. `CScriptSystem_Update` drains network commands and is
the only location that reads or mutates game entities. `CEntity::SetWorldPos` now operates on the
called entity rather than always moving the local player, and full position/quaternion transforms
are supported.

## Authority and limits

The server accepts the first finite transform as the starting point. Later updates must have a new
sequence number, a normalizable quaternion, finite position/velocity, and remain within
`15 m/s × elapsed time + 2 m`. Rejected movement receives a reliable `StateCorrection`.

The server derives animation state from horizontal velocity:

- below 0.15 m/s: Idle
- below 3.2 m/s: Walk
- otherwise: Run

Chat is limited to five messages per player per ten seconds and 256 UTF-8 code points.

There is no account system, persistence, Steam lobby, relay, NAT traversal, combat, damage,
inventory, interaction, horse, voice, NPC/quest/weather/world synchronization, delta compression,
or interest management in 0.1.

## Release blocker

The remote-human avatar risk gate is not satisfied, so Version 0.1 must not be presented as
complete.

The exact supported retail binary was audited:

```text
Steam build: 23914554
WHGame build: 1308617_856
TimeDateStamp: 0x6A350E20
SizeOfImage: 0x5B2D000
SHA-256: BDF8F9E4A11257A72B64C84700E284C29E4C4CCAF5B8D4BFA7D0B2A7294479F7
```

`wh_am_DebugPlayAnimation` is registered by code at RVA `0x01935BF4`, but its retail callback is
the empty stub at RVA `0x003B6E80` (`ret 0`). `cheat_spawn` does not exist in `WHGame.dll`; it is
provided by the script/cheat layer. Consequently, those command paths do not expose stable native
spawn and animation functions that can be typed and audited.

KCD2Online deliberately does not substitute a text command, Lua call, cloned brush, or debug marker.
The remaining gate is to identify and validate a native retail path that can:

1. create a fixed male human as a non-local entity;
2. disable AI, damage, collision, and quest relevance without disabling render/animation;
3. play stable Idle/Walk/Run states;
4. remove the entity without leaks.

Until that is demonstrated in-game, remote snapshots remain data-only and no remote entity is
spawned.

## Build and verification

`build.bat` bootstraps the pinned vcpkg commit under `.cache`, builds the DLL, server, audit tool,
and all KCD2Online tests with the `x64-windows-static` triplet.

Manual CMake configuration:

```powershell
cmake -S . -B out\build\debug -G "Visual Studio 18 2026" -A x64 `
  -D CMAKE_TOOLCHAIN_FILE="$pwd\.cache\vcpkg\scripts\buildsystems\vcpkg.cmake" `
  -D VCPKG_TARGET_TRIPLET=x64-windows-static `
  -D VCPKG_OVERLAY_PORTS="$pwd\cmake_scripts\vcpkg\ports" `
  -D BUILD_TESTING=ON -D FINAL=NO
cmake --build out\build\debug --config Debug --parallel
ctest --test-dir out\build\debug -C Debug -R "^KCD2Online" --output-on-failure
```

Automated coverage includes PE/signature resolution, engine paths, Protobuf round trips and
limits, authoritative server handshake/session/movement/chat behavior, bounded queue behavior,
and a real reliable/unreliable GameNetworkingSockets loopback exchange. CI validates Debug and
RelWithDebInfo.

The two-real-client, ten-minute acceptance run and remote avatar leak check cannot pass until the
avatar gate above is resolved.
