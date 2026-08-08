@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if not exist "server.toml" (
    if not exist "server.toml.example" (
        echo ERROR: server.toml.example is missing.
        pause
        exit /b 1
    )
    copy /Y "server.toml.example" "server.toml" >nul
    if errorlevel 1 (
        echo ERROR: Could not create server.toml from server.toml.example.
        pause
        exit /b 1
    )
    echo Created server.toml from server.toml.example.
    echo Edit server.toml to customize the server configuration.
)

if not exist "game_data\WHGame.dll" (
    echo.
    echo ERROR: The required game_data folder has not been generated yet.
    echo.
    echo Run KCD2OnlineGameDataGenerator.exe ONCE on a Windows PC where
    echo Kingdom Come: Deliverance II is installed. Then copy the generated
    echo game_data folder back next to KCD2OnlineServer.exe and run this file again.
    echo.
    pause
    exit /b 1
)

"%~dp0KCD2OnlineServer.exe" "%~dp0server.toml"
set "KCD2Online_SERVER_EXIT=%ERRORLEVEL%"
if not "%KCD2Online_SERVER_EXIT%"=="0" (
    echo.
    echo KCD2OnlineServer exited with code %KCD2Online_SERVER_EXIT%.
    pause
)
exit /b %KCD2Online_SERVER_EXIT%
