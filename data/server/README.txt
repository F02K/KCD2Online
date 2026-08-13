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
4. Run start_server.bat. On first launch it creates server.toml from
   server.toml.example automatically. Edit server.toml as needed. The default
   [property].game_data path already points at the generated folder beside it.

Run KCD2OnlineGameDataGenerator.exe again after a supported game update or after
changing installed mod PAKs so that the server content manifest stays current.
