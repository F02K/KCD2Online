# KCD2Online

Experimental multiplayer for Kingdom Come: Deliverance II.

> [!WARNING]
> KCD2Online **v0.1.4 is a prototype**, not a production-ready multiplayer mod.
> Expect breaking changes, incomplete world simulation, compatibility limits,
> and loss of multiplayer-world data while development continues. Use test
> saves and keep backups of anything important.

## Project status

| | |
| --- | --- |
| Current version | **0.1.4** |
| Development stage | Prototype / technical preview |
| Networking | Direct IP, dedicated authoritative server |
| Platform | Windows x64 |
| Supported game | Steam build `23914554`, game version 1.5 |
| Supported WHGame | `1308617_856` |

KCD2Online uses one project version across the client, server, build metadata, and
network handshake. During the prototype phase, clients and servers must run the
exact same KCD2Online version. There is no separate user-facing "protocol version".
See [CHANGELOG.md](CHANGELOG.md) for version history.

> [!CAUTION]
> NPC synchronization is still unreliable. A known bug can cause the same NPC
> to spawn multiple times, so NPC sync is not yet suitable for normal play.

## What works in v0.1.4

- Direct-IP client/server connection with authentication and reconnect support
- Native main-menu onboarding for the anonymous KCD2Online account service,
  with explicit consent and encrypted local credentials
- Persistent server sessions and player profiles
- Remote-player spawning, movement, appearance, equipment, and weapon state
- Native inventory and equipment reconciliation with rollback
- Server-authoritative doors and loot containers
- Supported containers: regular chests, cart chests, stash corpses, bird nests,
  and destructible stashes
- Server-authoritative dropped items, pickup ownership, and restart persistence
- Central item ledger with atomic player/container/world moves and validated
  stack split/merge accounting
- Shared time-of-day, time scale, and weather
- Independent human- and animal-NPC isolation controls
- Human/animal NPC transform and gameplay sync with spatial interest, a single
  renewable simulation lease, health/combat/Aggro, inventory, behavior intent,
  dialogue phase state, and canonical runtime-spawn identities
- Signature and Address Library validation before native hooks are enabled

Shared quests, exact dialogue-branch playback, schedules, crime/reputation,
and complete cooperative world progression are **not implemented yet**. NPC
dialogue synchronization currently covers active session/phase state; behavior
sync uses high-level intent and optional locomotion targets rather than copying
engine-private behavior-tree pointers.

The detailed implementation status and current limits are documented in
[docs/multiplayer.md](docs/multiplayer.md). The account consent, storage, and
current authentication boundary are described in
[docs/account-service.md](docs/account-service.md).

## Architecture

KCD2Online keeps its two runtime boundaries separate:

- `d3d12.dll` provides the mod-loader and ImGui frontend.
- `dinput8.dll` hosts KCSE.
- `KCD2OnlineKCSEClient.dll` owns the in-game multiplayer client and native game
  integration.
- `KCD2OnlineServer.exe` is a standalone dedicated server and does not load KCD2.

The project is based on
[KCD2ModLoader](https://github.com/xiaoxiao921/KCD2ModLoader) and
[ReturnOfModdingBase](https://github.com/xiaoxiao921/ReturnOfModdingBase).
It pins [F02K/libKCD2](https://github.com/F02K/libKCD2) and
[F02K/Address-Library-For-KCSE](https://github.com/F02K/Address-Library-For-KCSE)
as vendor dependencies.

Native engine access is capability-gated. A client join verifies the game
build, KCSE/libKCD2 runtime, Address Library identity, required native features,
content fingerprint, and KCD2Online version before entering the world. Runtime
objects never cross the frontend/client ABI boundary.

## Build and deploy

Requirements:

- Windows 10 or Windows 11
- Python 3.9 or newer
- CMake
- Visual Studio with the MSVC x64 C++ workload

Initialize the pinned vendor repositories after cloning:

```powershell
powershell -ExecutionPolicy Bypass -File tools/init_vendor.ps1
```

Run `build.bat` from the repository root. On first launch, the build tool
creates an isolated `.venv-build` environment and installs its pinned Python
dependencies.

The terminal UI can:

- build Debug or optimized Release artifacts with symbols;
- run native, protocol, server, networking, and deployment tests;
- discover the Steam installation or remember a manual game path;
- audit the installed `WHGame.dll` before deployment;
- validate and deploy the pinned Steam/GOG/Epic Address Library tables; and
- deploy both loaders and the KCSE client plugin.

Every successful build also creates a clean package tree under
`out/package/<debug|release>/`:

```text
client/   install-ready game tree and KCD2Online-Client-v0.1.4.zip
server/   dedicated server, configuration, data, symbols, and audit tool
tests/    test executables and their symbols only
SHA256SUMS.txt
```

When a local KCD2 installation is selected, the server package additionally
contains `game_data/WHGame.dll`, a hashed content manifest, generated human
Soul and authored human/animal NPC catalogs, and property catalogs for all
production levels. The DLL is audited before it is copied and is not included
in the client ZIP.

The client ZIP starts with `KingdomComeDeliverance2/` and mirrors the same
relative paths used by `Build & Deploy`. It can therefore be extracted directly
into the Steam `steamapps/common` directory. See
[Build and release packaging](docs/build-packaging.md) for the exact layout and
standalone packaging command.

`Build & Deploy` never starts or stops the game. Close KCD2 before deployment so
Windows can replace the runtime DLLs.

## Dedicated server

Copy `server.toml.example` to `server.toml`, select the sandbox `level_id`, set
the externally reachable `[auth].public_address`, then start the server:

```powershell
KCD2OnlineServer.exe server.toml
```

The default `[property].game_data = "game_data"` setting loads the generated
property catalog for the selected level. The dedicated server never needs the
original KCD2 installation path.

The common retail world IDs are:

- `2` — Trosky region (`trosecko`)
- `3` — Kuttenberg region (`kutnohorsko`)
- `4` — Monastery (`klaster`)

The server listens on UDP port `27020` by default. Allow and forward that port
only when hosting outside the LAN.

On its first start the server registers itself and writes its generated stable
ID and API key to `server-identity.json`. Keep that file with the server data.
It then publishes a browser heartbeat every 30 seconds. Only enabled/public
registrations with a fresh heartbeat are listed. Login tokens are bound to the
stable backend server ID and introspected before a player profile is loaded.

Persistent session data, player profiles, synchronized world objects, and
dropped items are stored below `world_directory`. Writes use temporary sibling
files followed by atomic replacement. Native save files are never uploaded to
or read by the dedicated server.

Available server commands include `status`, `players`, `kick`, `say`,
`profile claim`, `dummy spawn`, `dummy remove`, `entities`, `time`, `timescale`,
`weather`, `stop`, and `help`.

`dummy spawn [name]` creates a non-persistent simulated player from the current
starter profile. Equipped starter items use the normal remote-equipment path.
After a two-second spawn warm-up and short input buffers, the server sends brief
walk and turn inputs through ordinary transform snapshots. Translation stays at
the authoritative spawn point: the native client locomotion controller must move
and animate the actor, and every stop re-anchors it. This safely exercises the
same animation path as real player input without letting a test dummy wander off
terrain. Native weapon actions are excluded until their runtime path is verified.

## Joining from the game

1. Start KCD2 and choose **Multiplayer** in the main menu or pause menu. On the
   first visit, enable the anonymous KCD2Online identity or decline online play.
2. Select **Server**, **Server player name**, or **Password** to edit the value. Confirm with
   Enter, cancel with Escape, and paste with Ctrl+V.
3. Choose **Connect**. No savegame is required: from the title screen the
   server bootstrap starts KCD2's native New Game path directly in the configured
   level. From the pause menu, an already loaded matching level is adopted.

In game, `Enter` opens RP chat. Plain text is local `/say`; `/w`, `/y`, `/me`,
`/do`, and `/ooc` select whisper, shout, character action, scene description,
and global out-of-character chat. Hold `G`, point at an entry, and release it to
play one of the audited bow, cheer, point, or surrender emotes.

Proximity VOIP uses the Windows communications microphone. Hold `V` for normal
speech, `Ctrl+V` to whisper, or `Shift+V` to shout. Remote speech is decoded as
Opus and played through KCD2's running FMOD system as a 3D sound at the remote
player's head position.

Dedicated-server operators can bootstrap GM access with `permission grant
<player_id> admin.*`. Grants follow the player's persistent UUID and are stored
under the configured world directory. `/adminhelp` lists the in-game GM tools.

The in-game UI follows KCD2's current `g_language` setting. Editable UTF-8
translations are installed in `<game-root>\Mods\KCD2Online\Lang\`; English is the
fallback when no file exists for the selected game language. Native controls-page
labels are bundled separately as normal KCD2 localization PAKs.

The client waits for `NewGame`, `PreDataLoaded`, `DataLoaded`, the target
`wh_sys_BaseLevelId`, the local actor, and the native capability probe before it
applies the multiplayer profile and sends `WorldReady`. A different active level
is rejected until synchronized live travel is enabled. Failed native prerequisites
time out with a concrete loading phase instead of forcing the loading screen away.

## Manual installation

For manual deployment:

1. Copy `d3d12_.dll` beside `KingdomCome.exe` and rename it to `d3d12.dll`.
2. Copy KCSE's `dinput8.dll` beside `KingdomCome.exe`.
3. Copy `KCD2OnlineKCSEClient.dll` to
   `<game-root>\Mods\KCD2Online\KCSE\Plugins\`.
4. Copy the matching Address Library table to
   `<game-root>\KCSE\addresslib\`.
5. Copy the generated `Mods\KCD2Online\Lang`, `Localization`, and `mod.manifest`
   entries from the build output into `<game-root>\Mods\KCD2Online\`.

The default Steam binary directory is:

```text
KingdomComeDeliverance2\Bin\Win64MasterMasterSteamPGO
```

To uninstall the runtime, remove or rename `d3d12.dll`, `dinput8.dll`, and
`Mods\KCD2Online\KCSE\Plugins\KCD2OnlineKCSEClient.dll`.

## Diagnostics and testing

Debug and Release builds expose an output-only diagnostic console. Successful
startup reports the detected PE fingerprint, signature validation, and enabled
hooks. The build tool can run the same signature audit without starting KCD2.

Automated coverage includes protocol validation, server lifecycle and
persistence, player profiles, identity storage, remote avatars, container and
door conflicts, dropped-item ownership, environment state, native capability
gates, deployment behavior, and Address Library coverage.

In-game multi-client soak tests, save-directory comparison, fault injection,
and full NPC/quest simulation remain manual or future work.

## Additional documentation

- [Multiplayer architecture and status](docs/multiplayer.md)
- [Version history](CHANGELOG.md)
- [libKCD2/KCSE migration audit](docs/libkcd2-kcse-migration.md)
- [Vendor integration](docs/libkcd2-vendor.md)
- [Build and release packaging](docs/build-packaging.md)

## Existing mod-loader features

The inherited modding layer also provides Lua plugin loading and hot reload,
Dear ImGui Lua bindings, FMOD integration, ASI loading, XML merging, and debug
inspection tools. The example plugin is in
[`examples/plugins/KCD2Online-TestMod`](examples/plugins/KCD2Online-TestMod).
