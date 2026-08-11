# Server-Scripting mit Lua

KCD2Online lädt Ressourcen immer pro Server. Eine Ressource ist ein eigener Ordner unter `resources/` und besitzt genau eine `resource.toml`. Lua ist statisch in `KCD2OnlineServer.exe` und `KCD2OnlineKCSEClient.dll` eingebaut. Auf einem dedizierten Server werden daher weder Lua, das Spiel, KCSE noch das vollständige KCD2Online-Repository benötigt.

## Schnellstart aus der Server-ZIP

1. Einen neuen Ordner unter `resources/` anlegen.
2. `resource.toml` und mindestens einen Entry-Point unter `server/` oder `client/` erstellen.
3. Den Server neu starten. Ressourcen werden derzeit bewusst nur beim Start geladen; es gibt kein Hot-Reload.
4. Manifest- oder Ladefehler erscheinen in der Serverkonsole und verhindern den Start. Ausgaben von serverseitigem `print(...)` tragen das Präfix `[resource]`.

Für serverseitige UI oder Keybinds genügt ein `server/`-Ordner. Ein `client/`-Ordner ist nur nötig, wenn tatsächlich Lua auf dem Spieler-PC laufen soll.

## Ordnerstruktur

```text
resources/
  mein_script/
    resource.toml
    server/
      main.lua
      weitere_module.lua
    client/
      main.lua
    shared/
      daten.json
```

`server/` und `client/` sind optional. Eine rein serverseitige Ressource hat nur `[server]`; dann wird kein Code an Clients übertragen. Sobald `[client]` existiert, packt der Server ausschließlich `client/` sowie explizit freigegebene Dateien aus `shared/`. `server/` kann niemals Bestandteil des Client-Pakets werden.

## Manifest

```toml
[resource]
id = "mein_script"
version = "1.0.0"
api_version = 1
dependencies = []

[server]
entry = "server/main.lua"
capabilities = ["chat", "ui", "input", "players.kick"]

[client]
entry = "client/main.lua"
capabilities = []

[shared]
client_paths = ["shared/daten.json"]

[[events]]
name = "aktion"
direction = "bidirectional" # client_to_server | server_to_client | bidirectional
reliable = true
max_per_second = 10
max_bytes = 4096
```

IDs bestehen aus Kleinbuchstaben, Zahlen, `.`, `_` und `-`. Abhängigkeiten werden topologisch geladen; fehlende oder zyklische Abhängigkeiten stoppen den Serverstart. Entry-Pfade müssen in ihrem jeweiligen Ordner liegen. Symlinks, `..`, absolute Pfade und Lua-Bytecode werden abgewiesen.

## Server-API

- `server.on(name, callback)` registriert `start`, `player_joined`, `player_left`, `chat`, `player_death` oder `ui`.
- `server.players()` liefert eine Tabelle aus `{ id, name, connected, role }`.
- `server.say(text [, player_id])` sendet eine Systemnachricht. Erfordert `chat`.
- `server.kick(player_id [, reason])` trennt einen Spieler. Erfordert `players.kick`.
- `events.on(name, callback)` empfängt ein im Manifest erlaubtes Client-Event. Callback: `(player_id, payload)`.
- `events.emit_client(player_id_or_nil, name, payload)` sendet an einen Spieler; `nil` oder `0` sendet an alle. Richtung, Größe und Zuverlässigkeit stammen aus dem Manifest.
- `timer.after(ms, callback)` und `timer.every(ms, callback)` erstellen begrenzte Server-Timer.
- `ui.show`, `ui.patch`, `ui.close` und `ui.toast` benötigen `ui`.
- `input.register`, `input.unregister` und `input.on` benötigen `input`.

Die Lifecycle-Callbacks erhalten folgende Argumente:

| Name | Callback-Argumente |
| --- | --- |
| `start` | keine |
| `player_joined` | `player` |
| `player_left` | `player, reason` |
| `chat` | `player_id, text` |
| `player_death` | `player_id` |
| `ui` | `player_id, document_id, control_id, event, payload` |

Ein `player` enthält `id`, `name`, `connected` und `role`. IDs und Rollen sind serverseitige Informationen; Event- und UI-Payloads vom Client bleiben trotzdem untrusted input.

Beispiel für Lebenszyklus und Timer:

```lua
server.on("player_joined", function(player)
    server.say("Willkommen " .. player.name)
    timer.after(5000, function()
        server.say("Du bist seit fünf Sekunden verbunden.", player.id)
    end)
end)
```

`require("helfer")` lädt ausschließlich `server/helfer.lua` beziehungsweise auf dem Client `client/helfer.lua`. Dateisystem, Betriebssystem, Netzwerk, native DLLs, `loadfile`, `dofile` und dynamisches Bytecode-Laden sind nicht verfügbar.

## Client-API

Client-Lua ist für lokale Logik gedacht, nicht für Autorität. Es kann deklarierte Events mit `events.on(name, callback)` empfangen und mit `events.emit_server(name, payload)` senden. Der Empfangs-Callback erhält genau den JSON-Payload als Lua-Wert. Bereits beim Laden gesendete Events werden lokal zurückgehalten, bis der Server den Weltbeitritt akzeptiert hat. Der Server prüft Resource-ID, Event-Richtung, Rate und Payload-Größe erneut. `reliable = false` benutzt für beide Richtungen den unzuverlässigen Transport und eignet sich nur für ersetzbare Aktualisierungen.

Spielstände, Geld, Inventar, Positionen oder Berechtigungen dürfen niemals anhand einer Client-Aussage vertraut werden. Der serverseitige Handler muss Werte immer gegen den aktuellen autoritativen Zustand prüfen.

Eine Ressource braucht keinen `client/`-Ordner, um UI oder Keybinds zu verwenden: Die integrierte UI- und Input-Laufzeit ist bereits Teil des normalen KCD2Online-Clients. Das ist für die meisten Server-Skripte der empfohlene Weg.

## Fehler- und Ressourcenlimits

Jede Ressource besitzt einen eigenen Lua-State. Speicher, Instruktionen pro Callback, JSON-Tiefe/-Größe und wiederholte Laufzeitfehler sind begrenzt. Die Grenzwerte stehen in `server.toml` unter `[resources]`. Nach zu vielen Fehlern wird nur die betroffene Ressource deaktiviert. Ein Fehler beim initialen Laden stoppt den Server, damit kein halb konfigurierter Server online geht.

Weitere feste Grenzen sind 128 Ressourcen, 512 Dateien pro Client-Paket, 16 MiB pro Datei, 64 MiB pro Paket, 256 MiB für alle Client-Pakete und 32 KiB pro Event-/UI-JSON. Manifestwerte können Events zusätzlich kleiner und langsamer begrenzen.

Die mitgelieferten Beispiele `welcome_ui` und `event_example` können direkt kopiert, verändert oder gelöscht werden.
