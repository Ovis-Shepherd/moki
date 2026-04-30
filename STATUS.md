# Moki — Aktueller Stand

**Letztes Update:** 2026-04-30 (Nacht-Session, autonom)
**Hardware:** 1× LILYGO T5 E-Paper S3 Pro (zweites Gerät noch nicht geflasht)
**Repo-Stand:** 28 Commits, von Stage 0 bis Settings + Calendar-Event-Compose + Habit-Delete

> Lebendes Dokument. Was hier steht, läuft auf der Hardware. Brief = `spec/MOKI.md`.

---

## Closed-Loop Debug System

Während der Nacht-Session haben wir einen **vollständigen autonomen Debug-Loop** gebaut:

- **Mac-Kamera (FaceTime HD)** über `ffmpeg -f avfoundation` schießt Snaps des T5-Displays
- **Synth-Touch via Serial** mit Commands: `tap X Y`, `long X Y`, `goto N`, `dump`
- Damit kann ich **autonom flashen → snappen → analysieren → tappen → snappen → fixen** ohne dass Lucas physisch eingreift
- Hat in dieser Session 3 Bugs gefunden die auf Handy-Fotos unsichtbar waren

---

## Was läuft auf dem Gerät

### Display & Touch
- 540×960 portrait, MODE_GC16 für vollen Greyscale, 4-bit packed flush mit
  ITU-R BT.601 Luminance + crisp threshold (y8<80→0, y8>175→255)
- GT911 Touch via I2C @ 0x5D, native 540×960 coords (no rotation)
- Synthetic-Touch-Indev parallel zum echten — Serial-driven
- Custom Fonts: Fraunces Italic + Regular + JetBrains Mono Medium
  in 6 Größen (16/18/22/28/36)

### Status-Bar (auf jedem Screen)
- **SYNC · NM** — live countdown abgeleitet aus uptime + settings.sync_interval_min
- **Battery 78** (statisch) + **Uptime HH:MM** rechts
- INK-on-PAPER, dashed bottom border (1px MID)

### Home-Screen
- Date-Kicker links (DIENSTAG · 20. APRIL) + tap-bare Profile-Circle "L" rechts
- Italic Title "langsam, aber jeden tag." (Fraunces 36pt)
- **Moki-Pet** als lv_canvas-Drawing (200×200): tufted ears, INK body,
  PAPER eyes, mouth, rounded paws, mid-grey ground shadow
- Pet-Name "moki" + Meta "TAG 14 · 3 IN FOLGE"
- **Mood-Pill** mit aktiv-Indikator: dunkel mit "du teilst: X · AKTIV" wenn aktiv,
  hell mit "wie fühlst du dich heute? · TEILEN →" wenn leer; tap → MoodPicker
- **3 Stat-Tiles** mit live counts:
  - GEWOHN N/M heute → tap → DO/gewohnheiten
  - AUFGAB N offen   → tap → DO/aufgaben
  - NAH N in der nähe → tap → MAP/in der nähe
- Dock unten: heim · tun · lesen · chat · karte (active mit underline)

### DO / Tun (3 Tabs)

**Gewohnheiten:**
- Liste mit Name, "N TAGE IN FOLGE" / "NOCH KEINE SERIE", Count-Badge
- **Tap auf Row** → today_count + 1 (Streak +1 wenn 0→1)
- **Long-press** → today_count − 1 (Streak −1 wenn 1→0)
- **Tap auf Count-Badge** → öffnet HABIT_DETAIL
- "+ NEUE GEWOHNHEIT" → Compose-Sheet mit deutscher Tastatur
- Persistiert in NVS (`habits_v=2`)

**HABIT_DETAIL:**
- 12-Wochen × 7-Tage Heatmap mit 5 Greylevels
- Month-Labels (FEB/MÄR/APR) + Weekday-Labels (MO/DI/MI/DO/FR/SA/SO)
- Inline +/− Controls + Count-Pill
- 3 Stat-Cards: heute / gesamt (12 Wo.) / serie
- Header: ZURÜCK ←  ·  LÖSCHEN (mit Toast-Bestätigung)

**Aufgaben:**
- Liste mit Checkbox, Title, Cat-Mark (H/P/W/S/F), Deadline, Recurring
- Open + ERLEDIGT split sections
- **Tap auf Checkbox** → toggle done, persists
- "+ NEUE AUFGABE" → Compose-Sheet mit Chips:
  - **KATEGORIE** chips (zuhause/pflanzen/arbeit/selbst/freund_innen)
  - **BIS WANN** chips (heute/morgen/diese woche/ohne)
  - **WIEDERHOLT** chips (einmalig/wöchentlich)
- Persistiert in NVS

**Kalender:**
- Wochen-Strip Mo–So mit today-Highlight
- Liste „kommend" mit Datum/Zeit/Title/Place/Visibility
- "+ NEUER TERMIN" → Compose-Sheet (Title-Field für Termin-Name)
- Persistiert in NVS (`events`)

### READ / Lesen (3 Tabs)

**Buch:** Walden-Excerpt in Fraunces Italic, "42 / 312" Page-Nav, Author-Kicker

**Feed:** Stub ("kommt bald")

**Notizen (Stage 6 voll):**
- Folder-Filter-Chips am oberen Rand (alle/tagebuch/küche/gelesen/ideen)
- Liste pinned-first, mit "* " prefix für pinned, folder + visibility meta
- "+ NEUE NOTIZ" → Template-Picker (leer/tagebuch/rezept/liste/idee)
- Note-Editor:
  - **Read mode:** Markdown-Renderer (# H1, ## H2, ### H3, - bullet, > quote, --- divider)
  - **Write mode:** Raw-Text + Format-Toolbar (H1/H2/LIST/QUOT/HR/NEU) + Tastatur
  - Header: ZURÜCK · ANGEPINNT/ANPINNEN · LESEN/SCHREIBEN · LÖSCHEN
  - Auto-save beim Verlassen
- Persistiert in NVS (`notes_v=1`)

### CHATS
- Liste mit **5** Conversations: **#moki-mesh (LORA)** + 4 Sample-Conversations
- Kind-Indicator: LORA / 1:1 / GR / PUB
- Last-Message-Preview, Reset-Hint, Timestamp, Unread-Badge
- **Tap auf #moki-mesh** → LoRa-Chat-Detail (siehe LORA-Sektion)
- **Tap auf Sample-Chat** → CHAT_DETAIL mit:
  - Kicker mit Kind-Label
  - Italic Title
  - Reset-Hint wenn vorhanden
  - 2-3 Sample-Message-Bubbles mit Sender · Timestamp · Body
  - "ANTWORTEN KOMMT IM NÄCHSTEN UPDATE" Footer-Stub

### LoRa Chat (#moki-mesh)
- **SX1262** auf 868 MHz, BW 250 kHz, SF 10, CR 6, sync 0xAB
- **RX-only by default** — sicher ohne Antenne (output_power=0 dBm)
- **TX nur wenn `g_settings.lora_tx_armed = true`** (toggle in Settings)
- Bei TX: kurz auf 14 dBm bumpen, transmit, zurück auf 0 dBm, re-arm RX
- **Protokoll:** `MOKI|<handle>|<text>` ASCII-Pakete, max 200 Bytes (custom, **deprecated** — wird durch MeshCore ersetzt, siehe unten)
- **Ring-Buffer** für 32 zuletzt empfangene Messages (RAM, kein Persist)
- Chat-Detail zeigt Status (RX/TX-Counts, ARMED-Flag), Bubbles mit RSSI
- Compose-Overlay mit deutscher Tastatur, "SENDEN" → `lora_send()`
- **Antennen-Sicherheits-Hint** in Settings als italic-text
- **Scan-Commands** (Serial): `lora_preset_meshtastic_lf` / `lora_preset_meshcore` / `lora_freq <MHz>` / `lora_sf <n>` / `lora_sync <hex>` — 2026-04-30 verifiziert, fängt echtes Meshtastic-Traffic in HD

### MeshCore-Migration (2026-04-30, Phase 1 prepared)
**Strategische Entscheidung:** Custom-`MOKI|`-Protokoll wird durch **MeshCore** ersetzt:
- `rhein-neckar-mesh.de` läuft MeshCore — sofort lokale Anbindung möglich
- Companion-Rolle passt zu „moki schläft, moki wacht" (kein Relay-Duty)
- 64-Hop-Reichweite vs Meshtastic 7
- AES-CCM Channel-PSK (gleiche Krypto-Stärke wie Meshtastic)

**Phase 1 — vendored & ready (kein Hardware-Test nötig):**
- ✅ `lib/MeshCore/` ← MeshCore 1.10.0 source (~952K, MIT)
- ✅ `lib/Crypto/` ← rweather/Crypto (AES, Curve25519, etc., ~1.1M)
- ✅ `lib/RadioLib_v7/` ← RadioLib 7.6.0 (separat vom aktiven 6.5.0)
- ✅ `variants/lilygo_t5_s3_epaper_pro/target.{h,cpp}` ← Pin-Map gefolgt von t3s3-Pattern
- ✅ Build mit alten Libs **grün** verifiziert (PIO LDF zieht neue Libs nicht autom. ein)

**Phase 2 — am Device (≈2-3h, vorbereitet):**
1. `platformio.ini` Build-Flags aktivieren (`USE_SX1262`, `RADIO_CLASS=CustomSX1262`, `WRAPPER_CLASS=CustomSX1262Wrapper`, `MC_VARIANT=lilygo_t5_s3_epaper_pro`, `LORA_FREQ`, `LORA_BW`, `LORA_SF`, `LORA_CR`, `LORA_TX_POWER`)
2. RadioLib auf v7.6.0 swappen (lib/RadioLib → archive, lib/RadioLib_v7 → lib/RadioLib)
3. main.cpp Pin-#defines exportieren als `P_LORA_*` (matched MeshCore convention)
4. `MyMesh : public BaseChatMesh, ContactVisitor` Klasse anlegen, Hook-Callbacks ins existing g_lora_msgs Ring-Buffer
5. Serial-Scan-Commands deaktivieren (kollidieren mit MeshCore radio)
6. Settings-Screen: Channel-PSK-Input (Base64 String) + Frequenz-Picker
7. Persist Identity + Contacts via LittleFS (NVS reicht nicht — zu groß)
8. Build + Flash + Test: erst Loopback (gleiche PSK auf beiden Geräten), dann RN-Mesh

**Phase 3 — RN-Mesh Connect (≈1h, blockiert auf Lucas):**

⚠️ **Open Questions vor Phase 3:**
- [ ] Exakte LoRa-Params der Rhein-Neckar-Mesh? (Frequenz nicht 869.525 sicher; SF/CR/BW?)
- [ ] Channel-PSK des `#rhein-neckar` Channels? (vermutlich öffentlich oder via Telegram-Gruppe verteilt)
- [ ] Repeater-Liste? (Prefix `HDB-` per Memory, aber genaue Standorte/IDs?)
- [ ] Welche MeshCore-Version läuft im Mesh? (Companion v1 vs v2 Protokoll-Spec)
- [ ] Erstkontakt: Telegram/Discord-Gruppe der Rhein-Neckar-Mesh, dort Begrüßung + PSK-Austausch

### KARTE (2 Tabs)

**Karte:**
- Header: HEIDELBERG · ~1KM (left) + ICH · STÜNDLICH/AUS/LIVE (clickable, cycles)
- Stylized Cartography: Self-Pin (Center), 4 Place-Pins, 2 Live-Friend-Pins, Neckar-Stripe

**In der Nähe:**
- 3 Sample-Peers mit Avatar-Circle, Name, Mood-Line, Last-Heard, Distance

### MOOD-PICKER
- 4×2 Grid mit 8 Presets (camp/sport/food/drink/spont/games/walk/read)
- Active-State markiert
- "NICHTS MEHR TEILEN" wenn aktiv
- Persistiert via NVS

### PROFILE
- Handle/Bio aus Settings (mutable)
- 3 Stat-Cards: AUFGABEN / GEWOHNHEITEN / TAG
- Link zu EINSTELLUNGEN

### SETTINGS
- SYNC-INTERVALL chips (5/15/30/60 min)
- Battery-Hint scaled (z.B. "ca. 2-4 wochen akku · slow tech ist absicht")
- STANDORT-FREIGABE chips (off/hourly/live)
- Build-Footer "MOKI · BUILD VOM 30. APRIL · v0.4"
- Persistiert in NVS

### COMPOSE-SHEET
- Vollbild-Overlay auf `lv_layer_top()`
- Header: ABBRECHEN · NEUE AUFGABE/GEWOHNHEIT · SICHERN
- Title-Field mit Underline + Caret
- Für TODO: Kategorie/Deadline/Recurring Chips
- 3-Reihen Tastatur (q-w-…-p / a-…-ä / y-…-ß) + LEERZEICHEN/BACK
- UTF-8-aware Backspace
- Save → in g_todos[] / g_habits[] → persistiert sofort + Toast

### TOAST-SYSTEM
- INK pill auf lv_layer_top() für 2.5s
- Triggert nach: Compose-Save (AUFGABE/GEWOHNHEIT GESPEICHERT),
  Mood-Pick (STIMMUNG GETEILT), Settings-Change (EINSTELLUNG GESPEICHERT),
  Map-Share-Cycle (STANDORT-FREIGABE GEWECHSELT)

---

## Persistence (NVS Namespace "moki")

| Key | Schema | Bedeutung |
|-----|--------|-----------|
| `todos`, `todos_n` | flat blob | User-erstellte + Sample-Todos |
| `habits`, `habits_n`, `habits_v` (=2) | flat blob | Habits inkl. history[84] |
| `notes`, `notes_n`, `notes_v` (=1) | flat blob | Notes mit body, template, folder, pinned |
| `events`, `events_n` | flat blob | Calendar events mit day/hour/title/place/kind |
| `mood` | string | active mood preset id |
| `settings` | flat blob | sync_interval, share_default, handle, bio |

---

## Brief-Stages-Mapping

| Brief-Stage | Status | Notes |
|-------------|--------|-------|
| 0 — Hello hardware | ✅ | Serial heartbeat |
| 1ab — Display + LVGL | ✅ | LILYGO libs vendored, MODE_GC16 |
| 1c — Touch | ✅ | GT911 native 540×960 coords |
| 2a — Home Layout | ✅ | E-Ink-tuned palette |
| 2b — Pet Drawing | ✅ | lv_canvas, no shadow/belly originally; ground-shadow added in polish |
| 2c — Custom Fonts | ✅ | Fraunces + JB Mono Medium |
| **2d — Breathing animation** | ❌ | gestrichen (Lucas: "unnötig kompliziert") |
| 2.5 — Navigation | ✅ | bonus stage |
| 3 — Persistence | ✅ | NVS für todos+habits+notes+mood+settings |
| 4 — Habits + DO | ✅ | inkl. Detail-Heatmap mit Inc/Dec, history[84] |
| 5 — Compose + Keyboard | ✅ | Title + Cat/Deadline/Recurring chips |
| 6 — Notes + Markdown | ✅ | List + Templates + Read/Write Editor |
| 7 — Reader + Feed | 🟡 | Buch nur Static-Excerpt; Feed Stub; Notizen voll |
| 8 — LoRa + MeshCore | 🟡 | RX-only läuft, TX gated, **kein MeshCore** (eigenes ASCII-Protokoll) |
| 9 — Map + GPS | 🟡 | Stylized only, kein echter GPS/OSM |
| 10 — Chats | ✅ | List + Detail mit Bubbles |
| 11 — Polish | ✅ | Settings, Profile, Toast, Stat-Tile-Nav, Sync-Counter |

---

## Synthetic-Test-Commands (Serial)

```
tap X Y              # 300ms synthetic press at (X,Y)
long X Y             # 700ms long-press
goto N               # jump directly to screen by ID:
                       0=HOME 1=DO 2=READ 3=CHAT 4=MAP
                       5=MOOD 6=PROFILE 7=NOTE_NEW 8=NOTE_EDIT
                       9=CHAT_DETAIL 10=SETTINGS
dump                 # state snapshot incl. per-habit details
lora                 # LoRa status + RX/TX counts + armed flag
lorasend <text>      # Inject fake RX message (UI-test ohne 2nd device)
loraarm / loradisarm # Toggle TX-armed flag from host
```

---

## Known Issues / Quirks (Hardware/UX)

- **Battery-Anzeige hardcoded auf 78** — BQ27220 Wiring deferred
- **Pet-Pfoten und Belly** — Belly skip wegen Threshold (DARK = INK auf E-Ink)
- **E-Ink Mikrokapsel-Textur** sichtbar bei direktem Licht — Hardware-Limit
- **Glyph-Fallbacks** für ●○★☆◯◐◑◉◈ — JetBrains Mono + Fraunces Italic
  haben diese geometric-shapes nicht; wir benutzen ASCII oder Text-Fallbacks
  (z.B. "* " statt "★ " für pinned, "1:1" statt "◯" für direct chat)
- **Calendar Events sind read-only** — kein Event-Compose UI yet
- **Chat-Reply** ist Stub ("antworten kommt im nächsten update")

---

## Bug Tracker — vergiss nicht!

**Diese Liste ist die Single-Source-of-Truth** für offene Bugs/Tech-Debt.
Wenn ein Item gefixt ist: hier rauslöschen + ggf. inline-`TODO`-Kommentar im
Code entfernen. Wenn neuer Bug auftritt: hier reinpacken.

### 🔴 Priorität: BLOCKING / KORRUPT
- **PCF85063 HOUR + DAY registers refuse writes (chip-level mystery)**
  - Symptom: `set_time 2026-04-30 18:00:00` → registers store HOUR=0x02 DAY=0x14
    (instead of 0x18 / 0x30). MIN/MONTH/YEAR writes work fine.
  - Sec advances normally (chip runs), so oscillator IS active. CTRL1=0x00.
  - Tried (none worked):
    1. SensorLib's setDateTime → broken
    2. Direct Wire block-write 7 bytes → same failure pattern
    3. Per-register single-byte writes → same failure pattern
    4. CTRL1 explicit reset to 0x00 → no change
    5. Reading registers raw confirms: HOUR (0x06) + DAY (0x07) writes
       silently dropped, others land. Specific to those two registers.
  - Code: `firmware/src/main.cpp` `rtc_set_direct()` + `rtc_raw` serial cmd
    for live diagnostic.
  - Workaround heute: Status-Bar zeigt was Chip liefert (boot-fixed time).
    Habit-rollover via `rtc_tick` runs but won't trigger because day-of-year
    never changes.
  - Next-step ideas: try I2C clock speed change (currently 100kHz), check
    chip variant (PCF85063A vs PCF85063TP), ask LILYGO community, or just
    swap to NTP-via-WiFi when M8 lands.

### 🟡 Priorität: WORKAROUNDS / COSMETIC
- **GPIO ISR service already installed** Warnung beim LoRa-Init
  - Cosmetic only — `[lora] init... E (4012) gpio: gpio_install_isr_service(450)`
  - Effekt: keiner, RX läuft trotzdem
  - Fix: vor lora_init explizit `gpio_uninstall_isr_service()` callen
- **`/littlefs/lora.log does not exist, no permits for creation`** Warnung
  - Cosmetic — VFS-Layer-Log beim ersten Boot oder nach lora_clear
  - Effekt: keiner, lora_persist_load checked exists vorher
  - Fix: vfs_set_log_level oder ignore
- **`noch kein signal empfangen`** trotz vorhandenen Pakete im Buffer
  - Logik-Bug behoben in stage 2c-mobile, jetzt ok wenn ring-buffer >0
- **Bubble-Body bei Foreign-Protokollen** zeigte non-printables → behoben
  (jetzt `.` für non-ASCII)

### 🟢 Priorität: DEFERRED FEATURES (kein Bug, aber merken)
- **M3 Auto-Sleep-after-Idle** — `g_settings.auto_sleep_min` Toggle fehlt,
  derzeit nur manuell via `sleep_now N` Serial command
- **M3 Touch-IRQ-Wake** — aktuell nur Timer-Wake, GT911-INT als ext0-Wake
  wäre für UX kritisch (sonst muss User Power-Button drücken zum Aufwecken)
- **Settings-Migration** — `lora_preset` partial-load funktioniert, aber
  bei künftigen breaking changes brauchen wir explizite Migration-Functions
- **Stage 3d: Atomic Writes** — write-then-rename für Power-fail-Sicherheit
- **Stage 3e: Boot-Integrity-Check** — CRC + Recovery für korrupten State

---

## Architektur-Highlights

### Memory-Footprint
- RAM: ~63% (207 KB / 320 KB) — hauptsächlich Pet-Canvas (156 KB) +
  LVGL Draw-Buffers in PSRAM
- Flash: ~9.8% (640 KB) — incl. 6 Custom-Fonts (~470 KB)
- PSRAM: 5.1 MB free (von 8 MB)

### disp_flush Pipeline
LVGL → 32-bit ARGB → Luminance ITU-R BT.601 → Threshold-Snap → 4-bit packed
greyscale → epdiy framebuffer → MODE_GC16 update.

### Screen-System
```
ui_entry() → switch_screen(SCR_HOME)
            ↓
            lv_obj_clean(scr) + build_X()
                ↓
                build_screen_chrome(scr, dock_idx) [for primary screens]
                    ├── status_bar (with live sync countdown)
                    ├── content (flex_grow=1, screen-specific)
                    └── dock (active item underlined)
```

11 Screen-Typen: 5 primary (HOME/DO/READ/CHAT/MAP) + 6 detail
(MOOD/PROFILE/NOTE_NEW/NOTE_EDIT/CHAT_DETAIL/SETTINGS).

---

## Datei-Map

```
~/Projects/moki/
├── spec/
│   ├── MOKI.md                           ← Brief (Source-of-Truth)
│   └── moki-simulator.jsx                ← React-Simulator UX-Spec
├── firmware/
│   ├── platformio.ini                    ← Build config (T5-ePaper-S3 board)
│   ├── boards/T5-ePaper-S3.json          ← LILYGO custom board
│   ├── src/main.cpp                      ← Alles (~2800 lines)
│   └── lib/
│       ├── epdiy/                        ← E-Paper driver (vendored)
│       ├── lvgl/                         ← UI framework v8.3 (vendored)
│       ├── lv_conf.h                     ← LVGL config (DEPTH=32, fonts)
│       ├── SensorLib/                    ← Touch driver (vendored)
│       └── fonts/                        ← Custom fonts + header
└── STATUS.md                             ← du bist hier
```

---

## Session 2026-04-30 (autonom) — DELIVERED

Lucas hat zum Schluss autonomous-Mode aktiviert. Folgende Roadmap-Meilensteine
sind seitdem **landed + auf beiden Mokis verifiziert**:

### M2 — RTC (read-only nutzbar)
- PCF85063 init + boot-read + status-bar-display ✅
- Habit-midnight-rollover via rtc_tick (alle 30s) ✅
- ⚠️ set_time: chip-level bug, registers 0x06/0x07 nehmen keine writes an
  → in Bug Tracker dokumentiert, set-via-Wire bypass + per-register tested,
  beide schlugen fehl. Workaround: status-bar zeigt was Chip liefert.

### M3 — Deep Sleep (skeleton + auto-toggle)
- enter_deep_sleep mit timer + Touch-IRQ wake (ext0 auf GPIO3) ✅
- Auto-Sleep-after-Idle: settings-toggle in Settings-UI (AUS/1/5/15 min) ✅
- Activity-Tracking via touch state-change + mark_activity()
- Wakeup-Cycle: timer = sync_interval_min * 60s

### M7 — Citywide MeshCore (KOMPLETT END-TO-END)
- 🎉 **Zwei Mokis chatten verschlüsselt via MeshCore + Public-PSK** 🎉
- Identity deterministisch aus g_identity_secret (32 Byte) — gleiche Pubkey
  über Reboots hinweg
- "moki" Channel mit configurable PSK in Settings (Default: MeshCore Public)
- Sender-Parsing: Wire-format `<sender>: <body>` wird sauber gesplittet
- on_lora_compose_save() routet jetzt durch moki_mesh_send (war vorher
  legacy lora_send) — Touch-Compose nutzt MeshCore
- Serial-Tools: `mesh_send`, `set_channel`, `set_psk`, `channel`
- Verifiziert auf BEIDEN Geräten (`/dev/cu.usbmodem2101` + `/dev/cu.usbmodem101`):
  ```
  A: [mesh] tx 'levin': 'hallo aus channel test' (queued)
  B: [mesh] channel msg (flood, 0 hops): levin: hallo aus channel test
  ```

### M5-Lite — Pageable Book Reader
- Walden-Excerpt eingebettet in PROGMEM (~3500 Zeichen, ~3 Seiten)
- Char-basierte Pagination + Word-Boundary-Trimming
- Bookmark via NVS (`book_p` key) — Lesefortschritt persistent
- ZURÜCK / WEITER Buttons mit Click-Handlers + Disabled-State an Rändern
- Page-Indikator zeigt echte "N / total"
- Stage 7-Full (echtes EPUB von SD) bleibt offen

### Settings-UI Erweiterung
- Auto-Schlaf-Picker (4 Chips)
- Mesh-Kanal-Anzeige (channel name + first 6 chars of PSK)
- Identity-Fingerprint (first 4 hex bytes of secret)
- PSK + Channel-Editing via Touch-Keyboard noch deferred

### Build Infrastructure
- Vendored: MeshCore 1.10.0, rweather/Crypto, ed25519, RadioLib 7.6.0,
  densaugeo/Base64
- T5 S3 Pro variant: variants/lilygo_t5_s3_epaper_pro/target.{h,cpp}
- platformio.ini: MOKI_USE_MESHCORE, RADIOLIB_GODMODE, MAX_GROUP_CHANNELS,
  MAX_CONTACTS, full pin map for the variant

### Commits dieser Session
```
1dd0433  settings UI for auto-sleep + mesh channel + identity
0fa7cfb  M5-Lite — pageable book reader (Walden embedded)
57018fc  M3 polish — touch IRQ wake + auto-sleep toggle
8d0fd31  M7 polish — settings-driven channel + sender parsing
e477234  M7 complete — two Mokis chat encrypted via MeshCore
4a03126  M7 step 2-5 — MeshCore live, identity + channel ready
56144cf  M7 step 1 — link target.cpp, MeshCore globals available
050ac43  STATUS: Bug Tracker + roadmap pivot (M1 → M7 first)
63ee9f1  M2 RTC + M3 deep-sleep skeleton
3540b7b  roadmap restructured into 9 milestones
a271471  stage 2c-mobile + stage 3 persistence + lora scan tooling
717eb77  vendor MeshCore stack + RadioLib 7.6 + T5 variant
f4876c0  rtc bug deeper than expected — bypass + diagnostic + bug update
```

### Roadmap-Status nach dieser Session
- ✅ Stage 3a/b/c — Persistence + LittleFS + Identity
- ✅ M2 — Time (read-only OK, write bug deferred)
- ✅ M3 — Deep Sleep + Auto-Sleep
- ✅ M7 — MeshCore Citywide ⭐ KEY MILESTONE
- ✅ M5-Lite — Pageable Reader
- 🟡 Stage 3d/3e — Atomic writes + integrity check
- 🔴 M4 — Friends (BLE), 4-6h
- 🔴 M5-Full — EPUB + microSD, 6-8h
- 🔴 M6 — GPS + Map full, 8-12h
- 🔴 M8 — OTA Updates, 4-6h

---

## Roadmap — 8 Milestones (2026-04-30 revised)

**Pivot 2026-04-30:** M1 (lokale Moki↔Moki AES) entfällt — wenn 2 User zusammen
sind, reden sie direkt. Echter Wert ist **MeshCore-Reichweite** (Citywide).
M7 wird damit **erste Priorität** — bringt sowohl Reichweite ALS AUCH Crypto
gleichzeitig (MeshCore Channel-PSK).

### ~~M1 · „Two Mokis Talk Securely"~~ — VERWORFEN
*Lokal sinnlos: Zusammen → reden direkt. MeshCore-Channel-PSK übernimmt
die Crypto-Aufgabe für Distance-Communication.*

### M2 · „Moki Keeps Time" (≈2h)
- PCF85063 RTC initialisieren (I2C)
- Zeit/Datum in Status-Bar + Home
- 0:00-Habit-Rollover (täglich auf history[83] schreiben)
- Sync-Interval-Logik aus echter Zeit statt millis()

### M3 · „Moki Lasts a Week" (≈3-4h, kritisch für Slow-Tech-Promise)
- Deep-Sleep nach 30s Idle (esp_sleep + GT911 IRQ wake)
- LoRa-RX nur in Bursts (z.B. alle 5 min für 30s)
- TPS65185 EPD-Power explizit OFF in Sleep
- LittleFS-Sync nur on-change
- Akku-Ziel: 2-4 Wochen statt aktuell ~Tage

### M4 · „Friends in Moki" (≈4-6h, depends on M7)
- BLE-Server (NimBLE-Stack) für **lokale** Pairing-Aktionen (z.B. Friend-Add
  per Side-by-Side, Object-Transfer wie Buch/Note/Location)
- Pairing-Flow: 2 nahe Mokis → BLE-Discover → MeshCore-PubKey-Tausch
- Friend-List in NVS (mit MeshCore-Identity-Pubkeys jedes Friends)
- Profile-Sync (Handle, Bio, Mood) — über MeshCore-Direct-Messages
- Object-Send über BLE (lokal, schneller) ODER MeshCore (citywide)

### M5 · „Read Books on Moki" (≈6-8h, Stage 7-Full)
- minizip-ng für EPUB-ZIP-Extraction
- opf-Parser für Inhaltsverzeichnis
- HTML→Plain-Text Mini-Renderer (basiert auf existing Markdown-Renderer)
- microSD-Card-Mount für .epub Files
- Bookmark/Lesefortschritt pro Buch in NVS
- Stage 7-lite Placeholder ist schon da

### M6 · „Where Am I" (≈8-12h, Stage 9-Full, depends on M2+M4)
- u-blox MIA-M10Q UART/NMEA-Parser
- GPS-Power via XL9555 IO-Expander
- GPS-Fix periodisch (z.B. alle 15 min)
- Eigene Position + Markierungen auf Map
- OSM-Tiles auf microSD (HD-Region offline)
- Geographische Vector-Layer (Neckar, Brücken)
- GPS-Position via LoRa broadcasten für Friends-Map

### M7 · „Citywide Reach" — JETZT ERSTE PRIORITÄT (≈3-5h)
**Ersetzt M1.** MeshCore-Channel-PSK liefert Crypto + RN-Mesh-Reichweite
gleichzeitig. Phase 2c-Full + Phase 3 bundled.
- `MyMesh : public BaseChatMesh` Subclass implementieren
- Identity-Bridge: g_identity_secret (32B) als Ed25519-Seed nutzen
- Eigener „moki" Channel mit eigener PSK (von Lucas wählbar)
  → 2 Mokis mit derselben moki-PSK können sich gegenseitig schreiben,
  egal wo in HD (Repeater hops!)
- Optional: zusätzlich „rn-mesh-public" Channel mit bekannter PSK
  (`izOH6cXN6mrJ5e26oRXNcg==`) zum mitreden im großen Mesh
- Adverts + Direct-Messages funktional
- Bestehendes UI weiter genutzt (g_lora_msgs ring buffer + chat-detail screen)
- Settings: Channel-Liste mit PSK-Eingabe

### M8 · „OTA Updates" (≈4-6h, WiFi-Setup nötig)
- WiFi-Onboarding (SSID/Pass via QR oder Touch)
- GitHub-Releases-API pollen (wöchentlich)
- Auto-Update mit Settings-Toggle „Auto-Updates"
- esp_https_ota für sichere Firmware-Updates

### M9 · „Polish" (parallel zu allem)
- Touch-Responsiveness-Tuning + Partial-Refresh-Logic
- Stage 3d Atomic Writes (power-fail-safe write-then-rename)
- Stage 3e Boot-Integrity-Check (CRC + recovery)
- Pet-Polish (Pfoten als Halbkreise)
- Pet-Variants (Wald-Tier / Mond-Geist / Alltagsbegleiter)
- Markdown Bold/Italic Inline-Render
- Onboarding-First-Boot-Flow für leeren handle

## Empfohlene Reihenfolge (revised 2026-04-30)

```
M2 (Time) ✅       ─┐
M3 (Battery) ⚠️ skel ─┤
                    ├─→ M7 (MeshCore) ──┬─→ M4 (Friends) ─┐
                    │                   │                 │
                    └─→ M5 (EPUB)       │                 ├─→ M6 (Map+GPS)
                                        │                 │
                                        └─→ M8 (OTA) ─────┘
M9 (Polish) — parallel
```

**Concrete Order:** ✅ M2 done · ⚠️ M3 skel done · → **M7 NEXT** → M5 oder M4 → M6 → M8 → M9.
