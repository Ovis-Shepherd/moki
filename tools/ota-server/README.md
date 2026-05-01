# Moki OTA Server

Serviert die zuletzt gebaute `firmware.bin` aus dem PlatformIO-Build-Verzeichnis
über HTTP für Remote-OTA-Updates.

## Setup

```bash
cd tools/ota-server
bun run start
```

Server bindet auf `0.0.0.0:8080`. Zeigt deine LAN-IPs beim Start.

## Workflow

1. **Auf Mac:**
   ```bash
   cd firmware
   pio run                          # baut firmware.bin
   ```
2. **Server starten:**
   ```bash
   cd tools/ota-server
   bun run start
   # → "Serving on: http://192.168.1.42:8080"
   ```
3. **Auf Moki via Serial (oder einmalig direkt geflasht):**
   ```
   wifi_set MyHomeWifi MeinPasswort
   wifi_status              # wartet bis IP da
   ota http://192.168.1.42:8080/firmware.bin
   ```
4. Moki rebootet automatisch nach erfolgreichem Update.

## Endpoints

- `GET /health` → `{ok: true, service: "moki-ota"}`
- `GET /version.json` → `{size, sha256, build_time}`
- `GET /firmware.bin` → die rohe Firmware (rebuild manuell mit `pio run`)
- `GET /map` → moki.map aus `firmware/data/`

## Sicherheit

Aktuell **HTTP only, kein Auth**. Nur im LAN benutzen, oder per Tailscale o.ä.
auf Production-Mokis routen. Phase 2 bringt SHA256-Vergleich + ggf. HMAC-Signatur.

## Mobile / Remote Updates via GitHub Releases

Wenn du unterwegs bist (Handy-Hotspot statt Heim-WLAN), nutze GitHub statt
des lokalen Mac-Servers. Mac muss dann nicht mal laufen.

### Einmal-Setup

1. Repo auf GitHub anlegen, lokal als origin verknüpfen.
2. `gh auth login` — gh-cli authentifizieren (`brew install gh` falls fehlt).
3. Auf Moki einmal:
   ```
   ota_url https://github.com/<USER>/<REPO>/releases/latest/download/firmware.bin
   ```
   (URL bleibt in NVS gespeichert.)

### Ein Release publizieren

Vom Repo-Root:
```bash
./tools/release.sh                                # auto-bump patch (v0.1.x)
./tools/release.sh "fix LoRa SD-Race"              # auto-bump + custom notes
./tools/release.sh v0.2.0 "neue Map-Daten"         # explizite Version
```

Das Skript baut, erstellt einen Tag, lädt firmware.bin als Release-Asset hoch
und markiert es als `--latest`.

### Update auf Moki

Egal in welchem WiFi:
```
ota_release
```

Moki holt die `/releases/latest/download/firmware.bin` URL via HTTPS, schreibt
in OTA-Slot, rebootet.
