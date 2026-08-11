# Laden und Verteilen von Server-Ressourcen

Nach der normalen Versions- und Kontoanmeldung sendet der Server ein Manifest mit Generation, Root-SHA-256, Paketgrößen und den Hashes aller Client-Ressourcen. Der Client prüft seinen lokalen, inhaltsadressierten Cache und fordert nur fehlende Pakete in 48-KiB-Blöcken über den bereits verschlüsselten und zuverlässigen GameNetworkingSockets-Kanal an.

Jedes Paket wird vollständig gehasht, anschließend wird jede enthaltene Datei nochmals gegen ihren Hash geprüft. Erst danach wird es atomar im Cache gespeichert und aktiviert. Der Client bestätigt Generation und Root-Hash; erst dann sendet der Server den Welt-Bootstrap. Ein abweichender Hash, eine falsche Reihenfolge, ein zu großes Paket oder ein ungültiger Pfad beendet den Join.

Der Cache liegt unter `%LOCALAPPDATA%\KCD2Online\resources`. Pakete sind nach SHA-256 benannt und können serverübergreifend wiederverwendet werden. Eine Aktivierungsdatei ordnet einem Server nur die aktuell bestätigten Hashes zu. Änderungen erzeugen neue Hashes und werden beim nächsten Join automatisch geladen.

Wichtig: Der Client muss Lua nicht separat installieren. Der Server-Release enthält die statisch gelinkte Server-Laufzeit; der reguläre Client-Release enthält die Client-Laufzeit und den ImGui-Renderer. Ein Betreiber braucht aus dem Repository nur das, was in der Server-ZIP liegt: EXE, Konfiguration, `resources/`, Dokumentation und die lokal erzeugten `game_data`.

Servercode wird nie übertragen. Clientcode muss zwangsläufig auf dem Client ausführbar und damit grundsätzlich untersuchbar sein. Obfuskation wäre kein Sicherheitsmechanismus. Geheimnisse und autoritative Regeln gehören ausschließlich nach `server/`.

## Update- und Betriebsablauf

- Änderungen unter `client/`, an freigegebenen `shared/`-Dateien oder am Client-Manifest erzeugen einen neuen Paket- und Root-Hash.
- Rein serverseitige Änderungen erzeugen bewusst keinen neuen Client-Download.
- Nach einer Änderung den dedizierten Server neu starten. Bereits verbundene Spieler behalten den alten Zustand bis zur Trennung; ein laufendes Resource-Hot-Reload ist nicht vorgesehen.
- Beschädigte oder lokal veränderte Cache-Blobs werden beim nächsten Join verworfen und erneut geladen.
- Ein Resource-Ordner ohne `[client]` erscheint nicht im Download-Manifest.

Die Server-ZIP enthält `resources/` mit Beispielen und `docs/` mit diesen Anleitungen. Eigene Produktionsressourcen werden direkt in diesen Ordner kopiert; ein Repository-Checkout oder Build-System ist dafür nicht erforderlich.
