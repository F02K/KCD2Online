KCD2Online dedicated server
=======================

IMPORTANT: game_data is deliberately not included in the GitHub release ZIP.
It contains files copied and derived from a local Kingdom Come: Deliverance II
installation, including WHGame.dll, and must not be redistributed.

First-time setup
----------------

1. On a Windows PC where Kingdom Come: Deliverance II is installed, run
   KCD2OnlineGameDataGenerator.exe once.
2. The tool auto-detects Steam installations. If detection fails, enter the
   KingdomComeDeliverance2 installation directory when prompted.
3. Copy the generated game_data folder together with these server files to the
   machine that will host the dedicated server.
4. Run start_server.bat. On first launch it creates server.toml from
   server.toml.example automatically. Edit server.toml as needed. The default
   [property].game_data path already points at the generated folder beside it.

Run KCD2OnlineGameDataGenerator.exe again after a supported game update or after
changing installed mod PAKs so that the server content manifest stays current.

Lua resources
-------------

The release is self-contained: Lua is linked into KCD2OnlineServer.exe and no
separate runtime or SDK is required. Resources live in the resources folder.
The bundled examples include a server-only ImGui UI and a client/server event
resource. See docs\server-scripting.md, docs\resource-ui.md and
docs\resource-delivery.md before writing production scripts.
