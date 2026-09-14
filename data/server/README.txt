KCD2Online dedicated server
=======================

IMPORTANT: game_data is deliberately not included in the GitHub release ZIP.
It contains minimal compatibility metadata derived locally from a legitimate
Kingdom Come: Deliverance II installation. It contains no game executable,
DLL, PAK, XML, audio, image, or other source asset. Keep it private to the
server operator and do not publish it.

First-time setup
----------------

1. On a Windows PC where Kingdom Come: Deliverance II is installed, run
   KCD2OnlineGameDataGenerator.exe once.
2. The tool auto-detects Steam installations. If detection fails, enter the
   KingdomComeDeliverance2 installation directory when prompted.
3. If you host on another machine, privately copy the generated game_data
   folder together with these server files to that machine.
4. Run start_server.bat. On first launch it creates server.toml and
   dashboard.toml from their examples automatically. Edit them as needed. The
   dashboard listens only on 127.0.0.1:8080 and creates a private access token
   in dashboard-token.txt. The default [property].game_data path already points
   at the generated folder beside it.

Run KCD2OnlineGameDataGenerator.exe again after a supported game update or after
changing installed mod PAKs so that the server content manifest stays current.

Lua resources
-------------

The release is self-contained: Lua is linked into KCD2OnlineServer.exe and no
separate runtime or SDK is required. Resources live in the resources folder.
The bundled examples include a server-only ImGui UI and a client/server event
resource. Start with docs\scripting\README.md; its topic guides cover resource
structure, events, UI, keybinds, timers, security, delivery and complete examples.
