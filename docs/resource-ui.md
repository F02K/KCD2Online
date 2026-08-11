# Deklarative Server-UI und Keybinds

Server-UI ist ein validiertes JSON-Dokument, das der fest eingebauten ImGui-Schicht übergeben wird. Lua erhält keinen direkten ImGui-Zeiger und führt keinen Code im Render-Thread aus. Dadurch bleiben Rendering, Eingabe und Netzwerk voneinander getrennt.

Die vollständigen Signaturen lauten:

```lua
ui.show(player_id, document_id, document)
ui.patch(player_id, document_id, merge_patch)
ui.close(player_id, document_id)
ui.toast(player_id, toast_id, { text = "Nachricht" })
```

`player_id` stammt beispielsweise aus `player_joined` oder `server.players()`. Dokument-, Control-, Toast- und Action-IDs sollten stabile, kurze ASCII-Bezeichner wie `shop` oder `buy_item` sein.

## Ein Fenster anzeigen

```lua
ui.show(player_id, "shop", {
    title = "Haendler",
    position = { 80, 120 },
    size = { 440, 320 },
    movable = true,
    widgets = {
        { type = "text", text = "Warenangebot" },
        { type = "separator", text = "Auswahl" },
        { type = "checkbox", id = "bulk", text = "Mehrfachkauf", value = false },
        { type = "slider", id = "amount", text = "Menge", value = 1, min = 1, max = 10 },
        { type = "input", id = "note", text = "Notiz", value = "", },
        { type = "button", id = "buy", text = "Kaufen" },
        { type = "progress", text = "Ruf", value = 0.65 },
        { type = "spacer", height = 8 }
    }
})
```

Unterstützte Widget-Typen sind `text`, `separator`, `spacer`, `button`, `checkbox`, `slider`, `input` und `progress`. `same_line = true` setzt ein Element hinter das vorherige. Unbekannte Felder werden ignoriert; unbekannte Widget-Typen werden nicht gerendert.

`ui.patch(player_id, document_id, patch)` verwendet JSON-Merge-Patch-Semantik. `ui.close(player_id, document_id)` schließt das Fenster. `ui.toast(player_id, toast_id, { text = "Nachricht" })` zeigt die Meldung fünf Sekunden lang unten rechts an.

## UI-Ereignisse

```lua
server.on("ui", function(player_id, document_id, control_id, event, payload)
    if document_id == "shop" and control_id == "buy" and event == "click" then
        -- Preis, Bestand und Berechtigung immer serverseitig neu prüfen.
    end
end)
```

Buttons senden `click`, Checkboxen und Slider `change`, Textfelder bei Enter `submit`. Der Payload enthält beispielsweise `{ value = true }`. Die Dokumentrevision wird mitgesendet, ist aber kein Autoritätsbeweis.

`ui.patch` folgt JSON Merge Patch: Objektfelder werden ersetzt oder rekursiv zusammengeführt, `nil`/JSON-`null` entfernt ein Feld. Arrays wie `widgets` werden als Ganzes ersetzt.

## Keybinds

```lua
input.register(player_id, "open_shop", "Shop oeffnen", 0x75) -- F6

input.on("open_shop", function(player_id, payload)
    -- UI öffnen oder schließen
end)
```

`default_virtual_key` benutzt Windows-Virtual-Key-Codes von 1 bis 255; F8 (`0x77`) ist für den Editor reserviert. Mit `input.unregister(player_id, action_id)` wird die Aktion entfernt. Spieler öffnen mit F8 den lokalen Server-Keybind-Editor und können eine andere Taste wählen. Diese Belegung wird pro Server und Ressource unter `%LOCALAPPDATA%\KCD2Online\resource-keybinds.json` gespeichert. Tastendrücke werden nicht ausgelöst, während Chat oder ein Texteingabefeld aktiv ist.

Eine Action wird erst nach `input.register` auf dem Zielclient angeboten. `input.on(action_id, callback)` erhält `(player_id, payload)`; der aktuelle Payload ist `{ pressed = true }`. Feste Spielaktionen dürfen nicht allein davon abhängen, dass der Client diesen Key tatsächlich gedrückt hat.

Zum Schutz des Clients werden gleichzeitig höchstens 64 UI-Dokumente, 128 dynamische Bindings und 256 KiB deklarativer UI-Zustand gehalten. Nicht mehr benötigte Fenster und Actions sollten trotzdem immer mit `ui.close` beziehungsweise `input.unregister` entfernt werden.

## Sicherheitsmodell

UI ist niemals vertrauenswürdig. Ein veränderter Client kann jedes UI- oder Keybind-Ereignis selbst erzeugen. Der Server muss deshalb bei jeder Aktion Distanz, Zustand, Besitz, Preis, Cooldown und Berechtigung erneut prüfen. Hashes schützen den normalen Client vor beschädigten oder lokal veränderten Cache-Dateien; sie machen einen fremden, absichtlich manipulierten Client nicht vertrauenswürdig.
