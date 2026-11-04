# Vancouver 2010 (PC) — online spielen ohne Games for Windows LIVE

Kurzfassung für Mitspieler. Die vollständige Dokumentation ist englisch:
[README.md](README.md), [TROUBLESHOOTING.md](TROUBLESHOOTING.md),
[KNOWN-ISSUES.md](KNOWN-ISSUES.md).

Games for Windows - LIVE ist abgeschaltet. Dieses Repository ersetzt es durch
XLiveLessNess (XLLN), einen freien Ersatz: lokales Profil, Erfolge, LAN/LIVE-Lobby,
Session-Beitritt und Einladungen.

**Hier sind keine Spieldateien enthalten.** Du brauchst deine eigene
Vancouver-2010-PC-Version (1.0.0.1, `Vancouver.exe` 8.320.328 Bytes).

## Was geht — und was nicht

| Funktion | Stand |
|---|---|
| Einzelspieler, 4 Spieler an einem PC | geht |
| Start ohne DVD/ISO | geht (No-DVD-Patch) |
| Lobby-Liste über Radmin VPN / ZeroTier / Tailscale | geht |
| Beitritt zu einer Lobby aus der Liste | geht |
| Einladung senden/annehmen | geht, außer dem Absturz aus KNOWN-ISSUES #1 |
| Ein Event starten | **geht nicht** (KNOWN-ISSUES #2) |

## Installation

1. Vancouver 2010 normal installieren (eigene DVD oder eigenes Image).
2. Repository als ZIP herunterladen und entpacken.
3. `patch\install.cmd` per Rechtsklick **als Administrator** starten.
   - Profilname eingeben (max. 15 Zeichen).
   - „Direktverbindung zu einem Host?" mit Enter überspringen.
   - „Disc-Check entfernen?" mit **J** beantworten.
   - GFWL-Deinstallation mit Enter überspringen (stört nicht).
4. Spiel starten, **START** drücken, dann **Play On Line**.

## Zusammen spielen

1. Radmin VPN installieren, gemeinsames Netzwerk erstellen/beitreten.
2. **Beide PCs danach neu starten.** Ohne Neustart bleibt die Lobby-Liste leer — XLLN baut
   die Adapterliste beim Spielstart auf.
3. Beide brauchen **dieselbe `xlive.dll`**: 1.688.576 Bytes, MD5
   `5470398b9d74a9842e6b7143caeb92ca`.
   Prüfen:
   ```powershell
   (Get-FileHash "<Spielordner>\xlive.dll" -Algorithm MD5).Hash
   ```
4. Host: Play On Line → LIVE Lobby → Player Match → Host, Lobby offen lassen.
   Mitspieler: Play On Line → LIVE Lobby → Lobby in der Liste wählen → beitreten.

Wenn die Liste leer bleibt, als Rückfall (Spiel dabei geschlossen) in
`%LOCALAPPDATA%\XLiveLessNess\xlln-config-1.ini`:

```ini
xlln_direct_ip_connect_ip_port = 26.x.x.x:39001
```

## Wenn etwas nicht geht

**Erst sammeln, dann reparieren.** Doppelklick auf `tools\collect-crash-run.cmd` — das Skript
liest nur, braucht keine Admin-Rechte und schreibt `Desktop\vancouver-crash-<Zeitstempel>.zip`
mit allen Logs, Dumps, Ereignisprotokoll-Einträgen und Datei-Prüfsummen.

**Vor dem nächsten Spielstart ausführen**: XLLN überschreibt sein Log bei jedem Start, sonst ist
der abgestürzte Lauf verloren.

Log: `%LOCALAPPDATA%\XLiveLessNess\logs\xlln-debug-1.log` (UTF-16LE ohne BOM).

Häufige Fälle (Details auf Englisch in TROUBLESHOOTING.md):

| Meldung / Symptom | Ursache | Abhilfe |
|---|---|---|
| „Could not find the Vancouver 2010 disc" | Disc-Check | `patch\patch-nodvd.ps1 -GameDir "<Spielordner>"` |
| „another instance is already running" | kein Fehler | anderes Spielfenster schließen |
| Lobby-Liste leer | kein Neustart nach Radmin, unterschiedliche DLLs | neu starten, MD5 vergleichen |
| Beitritt sofort abgelehnt | andere/ältere `xlive.dll` | diese DLL auf beiden PCs |
| `msvcp140.dll` / `d3dx9_38.dll` fehlt | 32-Bit-Runtime fehlt | VC++ 2015-2022 **x86** + DirectX Juni 2010 |
| Joiner fliegt nach ~30 s raus | offener Fehler | KNOWN-ISSUES #2, keine Abhilfe |
| Absturz ~2 s nach Einladung | offener Fehler | KNOWN-ISSUES #1, Belege sammeln |

## Rechtliches

`xlive.dll` stammt von [XLiveLessNess](https://gitlab.com/GlitchyScripts/xlivelessness)
(LGPL-2.1); der vollständige Quellcode dieses Builds liegt in `source/`. Keine Spieldaten, keine
Microsoft-Redistributables. Keine Verbindung zu EA, SEGA, Eurocom oder Microsoft.
