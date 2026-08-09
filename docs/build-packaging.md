# Build and release packaging

Every successful build from `build.bat` produces a clean distribution tree at:

```text
out/package/<profile>/
```

The raw CMake configuration directory also contains the language files in the
runtime-relative layout `Mods/KCD2Online/Lang/`. This applies both to builds started
through `build.bat` and direct `cmake --build` invocations of the `KCD2Online`
target. Translation-only changes are copied again on the next build without
requiring the client DLL to be relinked.

`<profile>` is `debug` or `release`. Existing package contents are replaced only
after a new package has been staged successfully, so stale files from earlier
builds cannot leak into a release.

## Layout

```text
out/package/release/
|-- client/
|   |-- KingdomComeDeliverance2/
|   |   |-- Bin/Win64MasterMasterSteamPGO/
|   |   |   |-- d3d12.dll
|   |   |   |-- d3d12.pdb
|   |   |   |-- dinput8.dll
|   |   |   `-- dinput8.pdb
|   |   |-- Mods/KCD2Online/KCSE/Plugins/
|   |   |   |-- KCD2OnlineKCSEClient.dll
|   |   |   `-- KCD2OnlineKCSEClient.pdb
|   |   |-- Mods/KCD2Online/Lang/
|   |   |   |-- de.lang
|   |   |   |-- en.lang
|   |   |   `-- README.md
|   |   `-- KCSE/addresslib/
|   |       `-- kcd_addresslib_*.bin
|   `-- KCD2Online-Client-v0.1.3.zip
|-- server/
|   |-- KCD2OnlineServer.exe
|   |-- KCD2OnlineServer.pdb
|   |-- KCD2OnlineGameDataGenerator.exe
|   |-- start_server.bat
|   |-- README.txt
|   |-- server.toml.example
|   |-- starter_profile.toml
|   |-- npc_archetypes.json
|   |-- game_data/
|   |   |-- WHGame.dll
|   |   |-- content_manifest.json
|   |   |-- npc_archetypes.json
|   |   |-- npc_world_catalog.json
|   |   |-- property_catalog_2.pb
|   |   |-- property_catalog_3.pb
|   |   `-- property_catalog_4.pb
|   |-- tools/
|   |   |-- KCD2OnlineSignatureAudit.exe
|   |   `-- KCD2OnlineSignatureAudit.pdb
|   `-- KCD2Online-Server-v0.1.3.zip
|-- tests/
|   |-- KCD2Online*Tests.exe
|   `-- KCD2Online*Tests.pdb
`-- SHA256SUMS.txt
```

When the build tool has a selected KCD2 installation, it audits that exact
`WHGame.dll` before creating `server/game_data`. The generated content manifest
hashes the DLL, `Tables.pak`, every production `level.pak`, and installed mod
PAKs. The NPC world catalog gives authored humans and animals a stable
`level_id:entity_guid` identity and records animal-spawner metadata. Property
catalogs are generated for all three production levels.

The DLL copy is deliberately server-only and is never added to either release
ZIP.
It comes from the builder's local installation, so locally produced packages
containing it must not be redistributed unless the game publisher's terms
permit that. The loose local server tree still receives `game_data` exactly as
before. The server ZIP always omits the entire directory, even when it exists in
the local package tree.

The loose client tree and ZIP are built from the same deployment mapping used
by the build tool. Changing a deploy destination therefore changes package
generation through the same code path instead of requiring a second manually
maintained file list.

## Client ZIP

The archive root is `KingdomComeDeliverance2/`. A Steam user can extract the ZIP
into:

```text
<Steam library>/steamapps/common/
```

The archive then merges into the existing game directory without requiring
manual relocation or DLL renaming.

ZIP entries are sorted and use normalized timestamps and permissions. Given
identical input binaries, packaging produces identical archive bytes. The ZIP
contains the same runtime files, symbols, plugin, and Address Library tables as
the normal deploy operation.

## Server ZIP

`KCD2Online-Server-v<version>.zip` contains the dedicated server, symbols,
configuration examples, `start_server.bat`, diagnostic tools, and the standalone
`KCD2OnlineGameDataGenerator.exe`. It never contains `game_data`.

Before the first server start, run the generator once on a Windows PC with KCD2
installed. It auto-detects Steam or prompts for the game directory, audits the
installed `WHGame.dll`, and writes `game_data` next to itself. Transfer that
directory with the server files if the host is a different machine.

The default `[property].game_data = "game_data"` setting makes the server load
the generated `property_catalog_<level_id>.pb` on first use of a world. Runtime
property discovery therefore does not require the original KCD2 installation
or its `level.pak` files on the dedicated host.

`start_server.bat` creates `server.toml` from `server.toml.example` when needed
and refuses to start with a clear setup message while `game_data` is missing.

## Standalone packaging

Packaging can be repeated from an already completed CMake build without
recompiling:

```powershell
python tools/package_build.py `
  --build-dir out/build/release `
  --config RelWithDebInfo `
  --output out/package/release
```

The command supports Visual Studio multi-configuration output and Ninja
single-configuration output. Missing or ambiguous artifacts stop packaging with
an actionable error.

## GitHub Actions

The nightly workflow uploads the complete `client`, `server`, and `tests`
directory tree as one Actions artifact. GitHub Releases receive:

- the install-ready client ZIP;
- the install-ready dedicated-server ZIP; and
- `SHA256SUMS.txt`.

The test executables and all symbols remain available in the Actions artifact
without cluttering the normal end-user download list.
