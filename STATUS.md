# Moki — Aktueller Stand

**Letztes Update:** 2026-04-30
**Hardware:** 1× LILYGO T5 E-Paper S3 Pro (zweites Gerät noch nicht geflasht)
**Repo-Stand:** 14 Commits, von Stage 0 bis Habit-Detail-Heatmap

> Lebendes Dokument. Was hier steht, läuft auf der Hardware. Was gestrichen
> ist, war geplant aber zurückgestellt. Brief = `spec/MOKI.md`.

---

## Was läuft auf dem Gerät

### Display & Touch (Stage 0–1c · ✅)
- Boot über USB-CDC Serial (`moki alive · t=Xs · heap=… · psram=…`)
- 540 × 960 portrait Render, MODE_GC16 für vollen Greyscale
- 4-bit packed greyscale flush mit ITU-R BT.601 Luminance + crisp threshold
- GT911 Touch über I2C @ 0x5D, LVGL Pointer-Indev, native 540×960 Coords
- Custom-Fonts: Fraunces Italic/Regular + JetBrains Mono Medium in 4 Größen

### Home-Screen (Stage 2 · ✅)
- Status-Bar: SYNC · 12M (links), 78  14:32 (rechts)
- Date-Kicker: DIENSTAG · 20. APRIL
- Italic Title: „langsam, aber jeden tag."
- **Moki-Pet** als lv_canvas-Drawing (200×200): tufted ears, INK body,
  PAPER eyes, mouth-line, paws, no shadow/belly (E-Ink-Limit)
- Mood-Pill (clickable, aber Picker noch nicht gewired)
- 3 Stat-Tiles: GEWOHN 1/4 · AUFGAB 3 · NAH 3
- Dock unten: heim · tun · lesen · chat · karte (heim active)

### Navigation (Stage 2.5 · ✅)
- Dock-Tap wechselt zwischen 5 primären Screens
- Active-Item bekommt INK-Underline
- LVGL `lv_obj_clean(scr)` + Re-Build pro Switch
- Tabs in DO/READ/MAP: re-render in place wenn Tab gewechselt

### DO / Tun (Stage 4-lite + 5-lite + 3-lite + 4-habits + 4-habit-detail · ✅)
- Drei Tabs: **gewohnheiten · aufgaben · kalender**

**Gewohnheiten:**
- Liste mit Name, Streak-Dots `●●●○○`, Count-Badge
- **Tap auf Row** → today_count + 1, Streak +1 wenn 0→1
- **Long-press** → today_count − 1, Streak −1 wenn 1→0
- **Tap auf Count-Badge** → öffnet **HABIT_DETAIL**
- "+ NEUE GEWOHNHEIT" → Compose-Sheet
- **Persistiert via NVS** (`habits_v=2` schema)

**HABIT_DETAIL (✅):**
- 12-Wochen × 7-Tage GitHub-Commit-Style Heatmap (5 Grey-Levels)
- Legend WENIGER ▢▢▢▢▢ MEHR
- Inline +/− Controls + Count-Pill
- 3 Stat-Cards: heute / gesamt (12 Wo.) / serie
- "← ZURÜCK" zur Liste

**Aufgaben:**
- Liste mit Checkbox, Title, Cat-Mark, Deadline, Recurring
- Open + ERLEDIGT split sections
- **Tap auf Checkbox** → toggle done, persists
- "+ NEUE AUFGABE" → Compose-Sheet (gleiche Tastatur)
- **Persistiert via NVS**

**Kalender:**
- Wochen-Strip Mo–So, today highlighted
- Liste „kommend" mit Datum/Zeit/Title/Place/Visibility
- Read-only (Sample-Daten)

### READ / Lesen (Stage 7-lite · partial)
- Drei Tabs: **buch · feed · notizen**
- **Buch:** Walden-Excerpt in Fraunces Italic, "42 / 312" Page-Nav, Author-Kicker
- Feed + Notizen: "kommt bald"-Stub (deferred)

### CHAT (Stage 10-lite · partial)
- Liste mit 4 Sample-Conversations
- Kind-Glyph (◯ direct / ◑ group / ◉ public)
- Last-Message-Preview, Reset-Hint, Timestamp, Unread-Badge
- Read-only, kein Conversation-Detail (deferred)

### KARTE (Stage 9-lite · partial)
- Zwei Tabs: **karte · in der nähe**
- **Karte:** Self-Pin (Center), 4 Place-Pins, 2 Live-Friend-Pins, Neckar-Stripe
- **In der Nähe:** Liste mit Avatar/Name/Mood/Last-Heard/Distance
- Read-only, kein echter GPS, keine OSM-Tiles (deferred)

### Compose-Sheet (Stage 5-lite · ✅)
- Vollbild-Overlay (`lv_layer_top()`)
- Header: ABBRECHEN · NEUE AUFGABE/GEWOHNHEIT · SICHERN
- Title-Field mit Underline + Caret
- 3-Reihen Tastatur: q-w-…-p / a-…-ä / y-…-ß + LEERZEICHEN/BACK
- UTF-8-aware Backspace (ä/ö/ü als 1 Glyph löschen)
- Save → in g_todos[] / g_habits[] → persistiert sofort
- **Limit:** keine cat/deadline-Chips, keine Description, kein Recurring

---

## Was persistiert (NVS)

| Key       | Schema | Bedeutung |
|-----------|--------|-----------|
| `todos`, `todos_n` | flat blob | Alle Todos inkl. user-erstellte |
| `habits`, `habits_n`, `habits_v` (=2) | flat blob | Habits inkl. history[84] |

**Was NICHT persistiert (yet):**
- Kalender-Events (sample-data, read-only)
- Chats (sample-data, read-only)
- Pet-State (age, streak, mood)
- Profile (handle, pub_key, bio)
- Active-Mood

---

## Brief-Stages-Mapping

| Brief-Stage | Status | Notes |
|-------------|--------|-------|
| 0 — Hello hardware | ✅ | Serial heartbeat |
| 1ab — Display + LVGL | ✅ | LILYGO libs vendored, MODE_GC16 |
| 1c — Touch | ✅ | GT911 native 540×960 coords |
| 2a — Home Layout | ✅ | E-Ink-tuned palette |
| 2b — Pet Drawing | ✅ | lv_canvas, no shadow/belly |
| 2c — Custom Fonts | ✅ | Fraunces + JB Mono Medium |
| **2d — Breathing animation** | ❌ | **gestrichen** (Lucas: "unnötig kompliziert") |
| 2.5 — Navigation | ✅ | bonus stage |
| 3 — Persistence | 🟡 lite | Todos+Habits via NVS, Rest deferred |
| 4 — Habits + DO | ✅ | inkl. Detail-Heatmap + Inc/Dec |
| 5 — Compose + Keyboard | 🟡 lite | Title-only; Cat/Deadline-Chips deferred |
| 6 — Notes + Markdown | ⏳ | nicht angefangen |
| 7 — Reader + Feed | 🟡 lite | Buch nur Static-Excerpt |
| 8 — LoRa + MeshCore | ⏳ | nicht angefangen (HARDEST) |
| 9 — Map + GPS | 🟡 lite | Stylized only, kein echter GPS/OSM |
| 10 — Chats | 🟡 lite | Liste only, kein Conversation-Detail |
| 11 — Polish | ⏳ | Power-Mgmt, Settings, Onboarding |

---

## Known Issues / Quirks

- **Pet-Pfoten** sehen aus wie Quadrate (sind rounded-rect mit LV_RADIUS_CIRCLE,
  aber bei 24×14 nur leicht gerundet) — kosmetisch
- **Status-Bar Datums-Anzeige hardcoded** — DIENSTAG · 20. APRIL ist statisch,
  kein RTC-Hookup yet
- **E-Ink Mikrokapsel-Textur** sichtbar bei direktem Licht — Hardware-Limit, akzeptiert
- **Sync-Counter `SYNC · 12M`** ist hardcoded String (kein echter Sync läuft)
- **GT911 reportet Resolution 0×0** — Treiber liefert trotzdem brauchbare
  raw-coords, kein Bug
- **Brown-out detector deaktiviert** wegen Display-Power-Spike (`-DCONFIG_ESP_BROWNOUT_DET=0`)
- **Pet-Canvas in .bss** belegt 156 KB internal SRAM — keine PSRAM-Migration yet,
  aber Heap noch über 100 KB frei

---

## Architektur-Highlights

### disp_flush
LVGL → 32-bit ARGB → Luminance ITU-R BT.601 → Threshold-Snap → 4-bit packed
greyscale → epdiy framebuffer.

Threshold: y8 < 80 → 0 (pure black), y8 > 175 → 255 (pure white). Mid-Range
[80-175] passt durch als gray4 5-10. Diese Threshold kontrolliert Anti-Aliasing-Look
und limitiert die nutzbaren Greyscale-Tones (5 Levels in Heatmap mussten als
spezifische Hex-Werte gewählt werden).

### Screen-System
```
ui_entry() → switch_screen(SCR_HOME)
            ↓
            lv_obj_clean(scr) + build_X()
                ↓
                build_screen_chrome(scr, dock_idx)
                    ├── status_bar
                    ├── content (flex_grow=1)
                    └── dock
```

`build_X()` für jeden primären Screen (HOME, DO, READ, CHATS, MAP).
Drill-Down-Screens (HABIT_DETAIL) clearen ebenfalls scr und bauen ohne Dock.

### Compose-Sheet als Modal
Vollbild-Overlay auf `lv_layer_top()`. Beim Schließen `lv_obj_del()`. Keine
Stack-Verwaltung — nur "open / close one at a time".

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
│   ├── src/main.cpp                      ← Alles (~2400 lines)
│   └── lib/
│       ├── epdiy/                        ← E-Paper driver (vendored)
│       ├── lvgl/                         ← UI framework v8.3 (vendored)
│       ├── lv_conf.h                     ← LVGL config (DEPTH=32, fonts)
│       ├── SensorLib/                    ← Touch driver (vendored)
│       └── fonts/                        ← Custom fonts + header
└── STATUS.md                             ← du bist hier
```

---

## Nächste Schritte (Lucas-prio)

1. **Mood-Picker** — Tap auf Mood-Pill am Home → 8-Preset-Picker
   (`camp/sport/food/drink/spont/games/walk/read`) → persistiert active_mood
2. **Notes-Editor** (Stage 6) — Markdown-Editor in READ/Notizen
3. *(später)* Compose-Erweiterung mit Cat/Deadline/Recurring-Chips
4. *(später)* Habits 0:00-Rollover + RTC-Integration
5. *(später)* Polish: Pet-Pfoten, Date-Kicker dynamisch, Settings-Screen
