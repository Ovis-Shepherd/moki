# MOKI — Project Handover

> A slow-tech E-Ink companion device. This document is the complete brief
> for implementing the firmware on real hardware after extensive UX
> prototyping in a React simulator.

---

## 0. How To Use This Document

You are picking up a project mid-stream. There is no firmware code yet.
There is a complete React simulator at `moki-simulator.jsx` (in this
repo or provided alongside this document) which serves as the **canonical
UX specification**. Every screen, every interaction, every piece of state
shape in the simulator should be reproduced on the real device.

**Read order for getting up to speed:**

1. Section 1–3 (Vision, hardware, design DNA) — 5 min
2. Open `moki-simulator.jsx` and scan it once — 15 min
3. Section 6 (Data model) and 7 (Screens) as reference while coding
4. Section 10 (Implementation roadmap) tells you what to build first

**Important norm:** Do not invent features that are not in the simulator
or this document. The product's restraint is the point. If the user asks
for something new, build it deliberately, not by extrapolation.

---

## 1. Vision

Moki is a slow-tech E-Ink companion device — a small physical thing that
sits on a desk or in a coat pocket and helps its owner do the things that
make life feel like life: reading, building habits, keeping in real
contact with a small circle of friends, going outside, noticing the
world. It is explicitly designed to be the *opposite* of a smartphone:
no notifications, no infinite feeds, no engagement metrics, no
algorithmic recommendations.

The product manifests as a small E-Ink device shaped roughly like a
pocketable phone. It has a soft, paper-coloured display, a Moki — a
small drawn creature that is the user's mascot — and a curated set of
features. It communicates with other Moki devices in physical proximity
over LoRa radio, syncing only every 30 minutes by design. The slowness
is the feature, not the bug.

The user describing the project at the start put it like this: a "kleiner
Freund um Ruhe zu finden und Prioritäten neu zu finden" — a little
friend to find peace and rediscover priorities.

### What Moki is not

- Not a smartwatch. No fitness tracking, no notifications.
- Not a smartphone replacement. No camera, no apps store, no browser.
- Not a social network. No follower counts, no public timelines.
- Not gamified. There is a soft accessory-unlock system, but no points,
  no streaks-as-pressure, no leaderboards.
- Not a productivity tool in the GTD sense. Habits and todos exist,
  but the framing is "tun" (doing), not "achieving".

### What Moki is

- A friendly, breathing pet on a paper-textured screen.
- A habit tracker, todo list, calendar, reader, RSS feed, notes app,
  map of nearby friends, mood broadcast, and async chat — all under
  one roof, all designed to be used briefly and put down.
- A device that connects to a *small* circle of friends via LoRa
  (Meshcore protocol), letting them see each other's moods, share
  notes and recipes physically (NFC tap or BLE pairing), and exchange
  short messages that auto-erase periodically to lower the bar for
  posting.
- A thing you read books on, write a journal in, and forget about
  for hours at a time, on purpose.

---

## 2. Target Hardware

**Primary device:** LILYGO T5 E-Paper S3 Pro

**Specs:**
- ESP32-S3-WROOM-1 microcontroller
- 4.7" E-Paper display, 960×540 px, 16 grayscale levels
- Capacitive touch (GT911 controller)
- 8 MB PSRAM
- 16 MB Flash
- WiFi 802.11 b/g/n
- Bluetooth 5.0 LE
- SX1262 LoRa transceiver (433 / 868 / 915 MHz; we use 868 MHz for EU)
- u-blox MIA-M10Q GPS module
- PCF8563 RTC
- TPS65185 E-Ink driver
- BQ25896 battery management
- USB-C for flashing and charging
- MagSafe-compatible wireless charging coil
- Approximately phone-shaped form factor

**SD card:** the device has a microSD slot. **Note:** the ESP32-S3 does
NOT boot from SD card. Firmware lives in internal flash. SD is for
runtime data: EPUBs, offline OSM map tiles, backups, large assets.

**Hardware extension we may eventually want (not in MVP):**
A PN532 NFC module, attached via I2C, to enable physical "tap to share"
between two devices. This is an optional addition that requires a small
amount of hardware integration work. Skip for MVP, design the share
flow so it can plug in later.

**The user is in Heidelberg, Germany.** The MeshCore network there is
real and active under the name "Rhein-Neckar-Mesh", with a known
repeater node prefix `HDB-`. Default frequency 868 MHz, default group
channel `#rhein-neckar`.

---

## 3. Design DNA

This is the single most important section beyond the data model.
Everything visible on the device must respect these rules.

### Palette

Only four shades, all on a warm cream paper base. No colour. No bright
white.

```
PAPER  #e8e2d1   — background, the "paper" itself (warm cream)
INK    #1a1612   — primary text and active elements (warm black)
DARK   #3a342c   — secondary text, body copy
MID    #8a8373   — tertiary, captions, dividers
LIGHT  #c9c1a8   — subtle borders, dotted lines, disabled
```

These map onto 4 of the 16 grayscale levels of the panel. If ghosting
appears in practice, use epdiy's dithering on the mid tones rather than
adding shades.

### Typography

Two fonts, no exceptions:

- **Fraunces** (serif, italic available) — for body text, titles, content
  the user reads or writes. Books, notes, names, bios.
- **JetBrains Mono** — for chrome: timestamps, labels, status bar,
  uppercase metadata, button labels, anything mechanical.

Lowercase is the default everywhere. Uppercase only in the JetBrains Mono
chrome (where it's tracked at 0.15–0.25em for that ink-stamp feel).
Headings use lowercase italic Fraunces. Avoid sentence case for chrome.

For the firmware: bundle Fraunces and JetBrains Mono as `.ttf` files
on the device. LVGL can load them via lv_font_conv pre-rendered binary
fonts at the sizes we use (8.5, 9, 10, 13, 14, 16, 18, 22).

### Tone of voice

German, lowercase, gentle, never imperative. Examples:

- ✓ "wie fühlst du dich heute?"
- ✗ "Stimmung wählen"
- ✓ "noch keine serie"
- ✗ "Streak: 0"
- ✓ "moki lebt, solange du lebst. keine ängste. keine alarme."
- ✗ "Don't forget to feed your pet!"

Quotation marks are German („so").

### Iconography

Hand-drawn-feeling SVG icons with 1.6px stroke, round line caps, no fills
unless necessary. Replicate the simulator's icon set in firmware as LVGL
custom symbols or a small bitmap font.

Visibility uses three glyphs consistently throughout the app:
- `◯` — privat
- `◐` — freund_innen
- `◉` — öffentlich

Mood icons (camp, sport, food, drink, spont, games, walk, read) use
single Unicode glyphs (`⛺ ⚡ ◒ ◉ ✦ ◈ ◐ ☾`) — keep these as text glyphs in
firmware too if the bundled fonts include them, otherwise use custom
bitmap variants.

### Form language

- Buttons are rectangles with `border-radius: 2px`. Almost square.
- Active selections are filled INK with PAPER text.
- Inactive selections have a 1px dashed LIGHT border.
- Dividers are dotted (1px LIGHT), not solid.
- Status bar uses dashed bottom border.
- Active dock item underlines its icon (1.5px solid INK), not background.
- Streaks render as `●●●○○` not as numbers.
- Heatmaps use 5 ink intensities (see cellStyle in simulator).

### Animation philosophy

Three motions only. All else is static.

1. **Moki breathing** — gentle 5s scale 1.0 → 1.03 → 1.0 loop on the pet.
2. **E-Ink full-refresh flash** — 160ms ink overlay on screen change.
   On real hardware this maps to `epd_clear()` every ~8 partial refreshes.
3. **Sync pulse** — 1.2s opacity flicker on the sync indicator while
   the LoRa exchange is in progress.

That's it. No transitions, no slides, no fades. The device feels static
and the eye has time to settle on what it sees.

---

## 4. Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                      app                            │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐         │
│  │ screens  │  │  state   │  │ business │         │
│  │  (LVGL)  │←→│ (NVS+RAM)│←→│  logic   │         │
│  └──────────┘  └──────────┘  └──────────┘         │
└────────────┬─────────────┬─────────────┬───────────┘
             │             │             │
        ┌────▼────┐  ┌─────▼─────┐ ┌─────▼──────┐
        │ display │  │  storage  │ │ networking │
        │ (epdiy) │  │ (NVS+SD+  │ │ (MeshCore  │
        │  +touch │  │  LittleFS)│ │  +WiFi+BLE)│
        │  +GT911 │  └───────────┘ └────────────┘
        └─────────┘
```

### Layer boundaries

**Display layer** — owns the panel. Wraps epdiy for partial/full refresh
control and exposes a framebuffer to LVGL via a custom display driver.
Counts refreshes and triggers an `epd_clear()` every 8 partial refreshes
to combat ghosting. Touch input via GT911 over I2C is fed into LVGL's
indev system.

**Storage layer** — three tiers. `NVS` for small structured state (pet,
profile, sync metadata, settings). `LittleFS` on internal flash for
notes, chats, habit history (medium-sized blobs that need atomic writes).
SD card for large opaque data (EPUBs, OSM tiles, backups, thumbnail caches).

**Networking layer** — all sync is opportunistic, never blocking the UI.
Three transports:
- **LoRa via MeshCore** for the 30-minute sync (moods, presence, public
  chat, friend updates, shared notes/recipes/places). Built on the
  MeshCore Companion role; we don't relay other people's traffic.
- **WiFi** for RSS fetching, OSM tile updates, optional sync of larger
  shared content. Only when at home/known networks. Never blocking.
- **BLE** for one-off friend pairing (proximity-based). Eventually for
  NFC if hardware extension is added.

**App layer** — pure business logic. Operates on data structures, knows
nothing about the screen or radio. Everything touching the UI goes
through events (LVGL message bus) so the same handlers drive simulator
and hardware.

### Sync model — "moki schläft, moki wacht"

Default cycle: every 30 minutes the device wakes, exchanges sync data
with the nearest MeshCore Room Server, and goes back to deep sleep. The
status bar shows `sync · 12m` (countdown to next sync) and the user can
manually tap to trigger an immediate sync. The 30-minute cadence is
both a battery measure AND a product promise (slow tech).

In firmware this is an `esp_timer` that wakes the device from deep sleep,
runs `meshcore_sync()`, and re-arms. RTC time is preserved across sleep
via the PCF8563.

**Sync-policy refinement (decided 2026-04-29):** the slowness stays as
default but the user gets three levers, all in-spirit:

1. **Default interval 30 min** (unchanged, slow-tech promise).
2. **Wake-on-touch quick-sync:** any touch after >5 min since the last
   sync triggers an immediate sync before the screen finishes redrawing.
   Cheap on battery (radio runs ~2-3s), feels "fresh" without always-on.
3. **Settings interval 5 / 15 / 30 / 60 min:** user-adjustable in the
   settings screen. Shorter intervals cost battery transparently — the
   settings screen surfaces the trade-off (e.g. "5 min · ~3 tage akku").

Always-on push was considered and rejected: SX1262 RX-mode draws ~5 mA
continuously, which would collapse battery life from 2-4 weeks to a few
days, and LoRa is not a true push transport anyway — the receiving
peer's device must also be awake. The middle path keeps the calm
factor while letting the impatient user opt in.

### Power model

The device is mostly asleep. Wake triggers:
- Touch on screen (GT911 interrupt)
- Sync timer (every 30 min)
- USB power events
- Future: physical button press

When awake, draw the current screen, handle input, then return to deep
sleep on idle (~30s of no interaction). Estimated battery life with this
model: 2-4 weeks on the T5's onboard cell.

---

## 5. Toolchain

**Recommended:** PlatformIO inside VS Code. The user has not specified
their OS yet — ask if not stated. The setup is roughly:

1. Install VS Code
2. Install PlatformIO IDE extension
3. Create new project, board: `esp32-s3-devkitc-1`
4. Copy `platformio.ini` from section 9 below
5. Connect T5 via USB-C (CP210x or built-in USB-CDC)
6. `pio run -t upload` to flash, `pio device monitor` for serial logs

**Alternative:** ESP-IDF directly via command line. Same target chip,
more control, steeper learning curve.

**Key libraries:**
- `lvgl` — UI framework (use 9.x)
- `epdiy` — E-Paper driver, supports the T5 panel
- `meshcore` — LoRa mesh protocol (check current github for the C++
  variant compatible with ESP-IDF / Arduino-ESP32)
- `ArduinoJson` — for sync payloads
- `LittleFS_esp32` — filesystem
- Time/timezone via ESP-IDF builtin

---

## 6. Data Model

This is the single most important reference for porting. Every field name
below appears in the simulator's state and should appear identically in
the C structs and persisted state. Treat them as a contract.

```c
// Pet — the Moki creature itself.
typedef struct {
    char     name[16];           // "moki" by default
    char     mood[16];           // "calm" | "happy" | "curious"
    uint32_t age_days;
    uint32_t streak;             // current overall streak across habits
    struct {
        char ears[16];           // "tufted" | "pointed" | "round"
        char belly[16];          // "light" | "dark" | "none"
    } variant;                   // derived once from pub_key, immutable
    char     worn[16];           // accessory id or "" for nothing
    char     earned[8][16];      // up to 8 unlocked accessory ids
    uint8_t  earned_count;
} moki_pet_t;

// Profile — the user's identity, public-facing.
typedef struct {
    char     handle[32];         // "levin"
    char     pub_key[16];        // "HDB-3f2a" — derived from MeshCore identity
    char     bio[80];
    char     visibility[16];     // "private" | "friends" | "public"
} moki_profile_t;

// Habit — a thing the user does (potentially multiple times per day).
typedef struct {
    uint32_t id;
    char     name[48];
    uint32_t today_count;        // increments on tap, resets at midnight
    uint32_t streak;             // consecutive days with at least 1 count
    uint8_t  history[84];        // 12 weeks × 7 days, count per day
                                 // most recent day is index 83
} moki_habit_t;

// Todo — single task, optionally recurring.
typedef struct {
    uint32_t id;
    char     title[64];
    char     desc[128];
    char     cat[16];            // "home" | "plants" | "work" | "self" | "social"
    char     deadline[24];       // free-form: "heute" | "morgen" | "28. apr" | ""
    char     recurring[8];       // "" | "daily" | "weekly"
    bool     done;
} moki_todo_t;

// Calendar event.
typedef struct {
    uint32_t id;
    uint8_t  day;                // 0–6 day-of-week within current week (sketchy
                                 // for a real calendar — replace with real
                                 // datetime via PCF8563 + struct tm)
    char     hour[8];            // "19:00"
    char     title[64];
    char     place[48];
    char     kind[16];           // "private" | "friends" | "public"
} moki_event_t;

// Reader state — current open book.
typedef struct {
    char     book[64];
    char     author[48];
    uint32_t page;
    uint32_t total;
    char     epub_path[64];      // /sd/books/walden.epub
    char     last_excerpt[256];  // for offline display when book not loaded
} moki_reader_t;

// RSS feed item — small struct, kept in a ring buffer.
typedef struct {
    uint32_t id;
    char     src[24];
    char     title[80];
    char     date[16];
    bool     unread;
    char     url[128];
} moki_feed_item_t;

// Map — local stylized cartography state.
typedef struct {
    char     share[8];           // "off" | "hourly" | "live"
    float    self_lat;
    float    self_lng;
    // saved places
    struct {
        uint32_t id;
        float    lat;
        float    lng;
        char     name[32];
        char     kind[16];       // "saved" | "home"
    } places[16];
    uint8_t places_count;
    // friends sharing live location right now
    struct {
        char     id[16];          // pub_key e.g. "moki-7f2a"
        char     name[24];
        float    lat;
        float    lng;
        char     fresh[16];       // "3 min" | "12 min"
    } friends[16];
    uint8_t friends_count;
    // events with GPS
    struct {
        uint32_t id;
        float    lat;
        float    lng;
        char     title[64];
        char     hour[8];
    } events[8];
    uint8_t events_count;
} moki_map_t;

// Nearby — peers detected via LoRa, with their public profile bits.
typedef struct {
    char     id[16];              // pub_key
    char     name[24];
    char     pub_key[16];
    char     liked[80];           // last item they shared liking, or ""
    char     dist[16];            // "~80 m"
    char     mood[16];            // active mood id or ""
    char     last_heard[24];      // "im sync vor 3 min"
    char     bio[80];
    struct { char ears[16]; char belly[16]; } variant;
    char     worn[16];
    // public habits (mini grid on profile)
    struct {
        char    name[32];
        uint8_t history[84];
    } public_habits[4];
    uint8_t public_habits_count;
    char     public_books[8][48];
    uint8_t  public_books_count;
} moki_nearby_t;

// Mood broadcast — the one I'm sharing right now.
typedef struct {
    char     id[16];              // mood preset id
    uint64_t sent_at;             // unix ms
    char     expires[16];         // "today"
} moki_active_mood_t;

// Note — markdown body, with template origin and folder.
typedef struct {
    char     id[16];
    char     title[64];
    char     template_id[16];     // "blank" | "diary" | "recipe" | "list" | "idea"
    char     folder[24];          // "tagebuch" | "küche" | "gelesen" | "ideen" | ""
    char    *body;                // dynamically allocated, possibly large
    char     updated_at[24];      // "vor 2h" — derived display string
    uint64_t updated_at_unix;     // for sorting and "X ago" recomputation
    char     visibility[16];      // "private" | "friends" | "public"
    bool     pinned;
} moki_note_t;

// Note template — body is a markdown skeleton.
typedef struct {
    char     id[16];
    char     label[24];
    char    *body;
} moki_note_template_t;

// Chat — three kinds.
typedef struct {
    char     id[16];
    char     kind[8];             // "direct" | "group" | "public"
    char     name[32];
    char     members[8][16];      // pub_keys
    uint8_t  members_count;
    char     last[80];            // last message preview
    char     ts[16];              // "vor 8 min"
    uint8_t  unread;
    char     reset[8];            // "" | "daily" | "weekly"
    // messages live in LittleFS file: /chats/{id}.log, append-only,
    // truncated on reset interval
} moki_chat_t;

typedef struct {
    char     from[24];            // sender name or "unbekannt · HDB-22ee"
    char     text[256];
    char     ts[16];
    uint64_t ts_unix;
} moki_chat_msg_t;

// Sync metadata.
typedef struct {
    uint64_t last_at;             // unix ms
    uint32_t interval_sec;        // 1800
    char     room_server[16];     // "HDB-castle"
    int16_t  signal_dbm;
} moki_sync_t;

// Top-level state.
typedef struct {
    moki_pet_t          pet;
    moki_profile_t      profile;
    moki_habit_t        habits[16];      uint8_t habits_count;
    moki_todo_t         todos[64];       uint8_t todos_count;
    moki_event_t        calendar[32];    uint8_t calendar_count;
    moki_reader_t       reader;
    moki_feed_item_t    feed[32];        uint8_t feed_count;
    moki_map_t          map;
    moki_nearby_t       nearby[16];      uint8_t nearby_count;
    moki_active_mood_t  active_mood;     bool active_mood_set;
    moki_note_t         notes[128];      uint8_t notes_count;
    moki_note_template_t templates[16];  uint8_t templates_count;
    char                folders[8][24];  uint8_t folders_count;
    moki_chat_t         chats[32];       uint8_t chats_count;
    moki_sync_t         sync;
    uint8_t             battery;
    bool                wifi;
    bool                lora;
} moki_state_t;
```

### Persistence map

| Field | Storage | Notes |
|-------|---------|-------|
| `pet`, `profile`, `sync`, settings | NVS | Small, frequently read |
| `habits` | LittleFS `/state/habits.bin` | Daily increment + nightly rollup |
| `todos`, `calendar` | LittleFS `/state/tasks.bin` | Atomic write on change |
| `notes` (metadata) | LittleFS `/state/notes.bin` | One blob with all metadata |
| `notes` (body) | LittleFS `/notes/{id}.md` | One file per note |
| `chats` (metadata) | LittleFS `/state/chats.bin` | |
| `chats` (messages) | LittleFS `/chats/{id}.log` | Append-only, reset-truncated |
| `feed` cache | LittleFS `/state/feed.bin` | Rotates, max 32 items |
| `nearby` | RAM only | Rebuilt every sync |
| `active_mood` | NVS | Survives reboot |
| Books | SD card `/sd/books/*.epub` | |
| OSM tiles | SD card `/sd/maps/*.bin` | Pre-baked vector data |

### Mood preset IDs (fixed enum, must match simulator)

```c
// camp, sport, food, drink, spont, games, walk, read
const char* MOOD_IDS[] = {"camp","sport","food","drink","spont","games","walk","read"};
const char* MOOD_LABELS[] = {"campen","sport","essen gehen","bierchen",
                              "spontan","brettspiele","spazieren","still lesen"};
const char* MOOD_HINTS[] = {"mal wieder raus","tischtennis · rad","wer mag mit",
                             "einfach quatschen","alles möglich","heute abend?",
                             "mit gespräch","im café, gemeinsam"};
```

### Accessory unlock conditions

```c
typedef struct {
    const char *id;
    const char *label;
    const char *unlock_text;
    bool (*condition)(const moki_state_t *s);
} accessory_t;
```

| ID | Label | Unlock |
|----|-------|--------|
| `glasses` | lesebrille | nach 5 büchern gelesen |
| `leaf` | blatt | 30 spaziergänge am stück |
| `book` | büchlein | nach 100 habit-tagen |
| `hat` | mützchen | 1 monat dabei |
| `scarf` | schal | 10 mood-antworten geteilt |

---

## 7. Screen Inventory

The simulator has these screens. Each maps to one `lv_obj_t*` screen
object. Reference the simulator code section comments — they each begin
with `// ===` and are clearly labeled.

### Primary screens (in dock)

1. **HOME** — date, breathing Moki, mood broadcast pill, three stat
   tiles (gewohn / aufgab / nah), profile button top right.
2. **DO** (`tun`) — three tabs: gewohnheiten, aufgaben, kalender.
   - HABITS list with tap-to-increment (count badge), tap badge to detail.
   - TODOS list grouped open/done, with category mark, deadline, ↻ recur.
   - CALENDAR inline: 7-day strip + upcoming events.
3. **READ** (`lesen`) — three tabs: buch, feed, notizen.
   - BOOK: current chapter excerpt, page count, prev/next.
   - FEED: RSS items with source, date, unread dot.
   - NOTES: folder filter chips, list of notes (pinned first), + neue notiz.
4. **CHATS** — list of conversations (direct, group, public).
   - Tap → CHAT screen (single conversation).
5. **MAP** (`karte`) — two tabs: karte, in der nähe.
   - MAP: stylized vector cartography, pins for self/friends/places/events.
   - NEARBY: list of LoRa-discovered peers with last-heard times.

### Secondary / drill-down screens

- **HABIT_DETAIL** — 12-week × 7-day GitHub-style heatmap, stats.
- **CHAT** — single conversation thread, reset countdown if applicable.
- **FRIEND** — public profile of a nearby peer with mini grids.
- **MOOD** — picker for active mood broadcast.
- **PROFILE** — the user's own public profile preview.
- **MOKI** — pet detail with wardrobe (accessory slots).
- **NOTE_NEW** — template picker for new note.
- **NOTE_EDIT** — markdown editor with read/write toggle, format toolbar.
- **COMPOSE_SHEET** — overlay for new todo / habit / event with on-screen
  keyboard.

### Status bar (always on top)

- Left: tappable sync indicator with countdown `sync · 12m` or
  `synct …` (animated) during exchange.
- Right: lora icon, wifi icon (if connected), battery `78`, time `14:32`.

### Dock (bottom, hidden on drill-down screens)

5 slots: `heim · tun · lesen · chat · karte`. Active slot shows icon
underlined in INK.

Drill-down screens hide the dock and show a small `← zurück` instead
(or, in the chat, just a `←` arrow inline with the title).

---

## 8. Interaction Notes

### Habits are multi-count per day

Critical: a tap on the habit name increments `today_count`. Tap on the
count pill on the right opens HABIT_DETAIL. The streak only increments
when today_count goes from 0 to 1 (first count of the day). At local
midnight (RTC), today_count is appended to the history array (shift left
by one cell, drop oldest, set today's slot to 0) and the new today's
slot starts fresh.

### Sync flash & toasts

When the user manually triggers sync, animate the sync icon with
`sync-pulse` for ~1.8s while the LoRa exchange happens. On completion,
show a small toast at the bottom (`"sync · 2 neue moods"`) for ~2.2s.
Toasts also appear for compose saves, mood changes, etc. Position toast
above the dock if visible, otherwise near the bottom.

### Compose sheet (todo / habit / event creation)

Full-screen overlay with header (abbrechen / titel / sichern), form
fields, on-screen keyboard. Field focus is shown with a thicker
underline. Keyboard taps insert into the focused field. The "sichern"
button is disabled (low opacity) until the title is non-empty.

Categories, deadlines, recurring options, and visibility are tappable
chips, not text fields.

### Note editor — markdown live preview model

This is the most complex screen. Two modes toggled by a button top right:

- **Read mode (default for non-empty notes):** body renders as formatted
  markdown. Only headers, lists, todos, bullets, numbered lists, blockquotes,
  bold, italic, and `---` dividers are supported. No nested lists, no
  links, no images.
- **Write mode:** body becomes a textarea showing raw markdown. A
  formatting toolbar appears at the bottom above the keyboard with
  buttons that insert at cursor: H1, H2, bullet, todo, bold, italic,
  quote, divider.

Title field is always editable inline (no mode switch).

Pin toggle (★), visibility cycle (privat / freund_innen / öffentlich),
folder picker (popover), and delete (with confirm dialog) all live in
the meta strip below the title.

### Chat reset visualization

If a chat has `reset: "daily"` or `reset: "weekly"`, show under the chat
title: `"noch 14h · dann leer"` or `"noch 3 tage · dann leer"`. Compute
from current time and reset cadence. The actual deletion happens in a
nightly job that truncates `/chats/{id}.log`.

### Map share modes

The "ich · stündlich" pill cycles through `off → hourly → live` on tap.
Live mode shows a pulsing ring around the user's pin (the `ping-ring`
animation in the simulator). Live mode means a position broadcast every
sync window; off means never; hourly is the default.

---

## 9. Recommended `platformio.ini`

```ini
[env:t5_s3_pro]
platform = espressif32@^6.5.0
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
upload_speed = 921600

board_build.partitions = huge_app.csv
board_build.flash_mode = qio
board_build.f_flash = 80000000L
board_build.psram_type = opi

build_flags =
    -DBOARD_HAS_PSRAM
    -DCORE_DEBUG_LEVEL=3
    -DLV_CONF_PATH="${PROJECT_DIR}/include/lv_conf.h"
    -DLV_LVGL_H_INCLUDE_SIMPLE
    -DEPD_DRIVER_T5_PRO=1
    -DUSE_HSPI_PORT
    -mfix-esp32-psram-cache-issue
    -fexceptions

build_unflags =
    -fno-exceptions

lib_deps =
    lvgl/lvgl @ ^9.1.0
    bblanchon/ArduinoJson @ ^7.0.0
    lorol/LittleFS_esp32 @ ^1.0.6
    https://github.com/vroland/epdiy.git
    ; meshcore — pin to a known-good commit:
    ; https://github.com/ripplebiz/MeshCore.git
```

A custom partition table (`partitions.csv`) reserving ~4MB for app and
~10MB for LittleFS, with the rest for OTA, makes sense.

---

## 10. Implementation Roadmap

Build in stages, each ending with something runnable on real hardware.
Do NOT attempt to port everything at once.

### Stage 0 — Hello, hardware (1 session)

Goal: confirm the toolchain works and you can flash the device.

- [ ] Set up PlatformIO project from `platformio.ini` above.
- [ ] Add minimal `main.cpp` that initializes serial, prints "moki alive"
      every second.
- [ ] Connect T5 via USB-C, flash, see output in serial monitor.
- [ ] Stop here until this works. If it doesn't, debug toolchain before
      continuing.

### Stage 1 — Display & touch (1–2 sessions)

Goal: render something on the E-Ink screen, react to a touch.

- [ ] Initialize epdiy with the T5's panel parameters.
- [ ] Initialize LVGL with a custom display driver pointing at epdiy.
- [ ] Initialize GT911 over I2C and feed events into LVGL's indev.
- [ ] Draw a single "moki alive" label in Fraunces (start with default
      LVGL font; we'll bundle Fraunces later).
- [ ] Make a button that, when tapped, swaps the label.
- [ ] Verify the partial-refresh + occasional full-refresh strategy
      doesn't ghost too badly.

### Stage 2 — Moki pet & home screen (2–3 sessions)

Goal: home screen lookalike, static for now.

- [ ] Bundle Fraunces and JetBrains Mono via lv_font_conv.
- [ ] Render Moki pet as a vector drawing using `lv_canvas` (translate
      the SVG paths from the simulator) OR pre-render to a 130×130
      grayscale bitmap and use that.
- [ ] Lay out the home screen elements: date, italic heading, pet,
      mood pill, three stat tiles. Match the simulator pixel-for-pixel
      where possible.
- [ ] Add the breathing animation (`lv_anim_t` on the pet's transform).
- [ ] Add the status bar at the top, dock at the bottom (placeholder
      navigation that doesn't yet switch screens).

### Stage 3 — Persistence & data model (2 sessions)

Goal: state survives reboot.

- [ ] Implement the C structs from section 6.
- [ ] Write NVS read/write for `pet`, `profile`, `sync`, settings.
- [ ] Write LittleFS read/write for habits, todos, notes metadata.
- [ ] Bootstrap with the default state (matching the simulator's
      `createInitialState`) on first boot.
- [ ] Verify reboot preserves state.

### Stage 4 — Habits + DO screen (2–3 sessions)

- [ ] Build the DO screen with three tabs.
- [ ] Implement habit increment + streak calc.
- [ ] Implement HABIT_DETAIL with the 12-week heatmap.
- [ ] Implement TodoList (no compose yet, just static todos).
- [ ] Implement CalendarInline (read-only).
- [ ] Add midnight rollup task (timer based on RTC time).

### Stage 5 — Compose sheet & keyboard (2 sessions)

- [ ] Build the on-screen keyboard layout (LVGL `lv_keyboard` with
      custom layout — German letters incl. ä ö ü ß).
- [ ] Build the ComposeSheet overlay with form fields.
- [ ] Wire compose → save → state update → list refresh.
- [ ] At this point users can create habits, todos, events on device.

### Stage 6 — Notes & markdown (3 sessions)

- [ ] Implement the markdown renderer in C — line-based, supporting
      H1/H2/H3, bullets, numbered lists, todos `- [ ]` `- [x]`,
      blockquotes `> `, bold `**`, italic `*`, divider `---`.
- [ ] Build NotesTab list with folder filter chips.
- [ ] Build NoteEditorScreen with read/write toggle.
- [ ] Build the format toolbar above the keyboard.
- [ ] Build TemplatePickerScreen.
- [ ] Implement folder picker popover, delete confirm dialog.

### Stage 7 — Reader & feed (2 sessions)

- [ ] EPUB parser (use a small embedded library, or write a minimal
      one that extracts the first text content). Books on SD card.
- [ ] BookTab renders one screen of book text at a time.
- [ ] Persist current page per book.
- [ ] FeedTab renders cached RSS items. RSS fetch is a WiFi job that
      runs opportunistically.

### Stage 8 — LoRa & sync (3–4 sessions)

This is the hardest stage. Take it slowly.

- [ ] Integrate MeshCore Companion role.
- [ ] Implement the sync payload format (JSON over LoRa, ~600-800 bytes
      per device per sync).
- [ ] Sync timer: every 30 min, wake → sync → sleep.
- [ ] Process incoming sync data: update `nearby`, `chats`, public
      shared notes/recipes, friend mood changes.
- [ ] Send local outgoing data: my mood, my public habits, my live
      position (if share mode is `live`), my queued chat messages.
- [ ] Status bar countdown becomes real.

### Stage 9 — Map (2 sessions)

- [ ] Bake offline OSM vector data for Heidelberg into a binary blob
      stored on SD card. Minimum: rivers, major roads, parks, district
      polygons.
- [ ] Render the map by projecting these features around the user's
      live GPS position.
- [ ] Add place pins, friend pins, event pins, self pin (with ping).
- [ ] Implement share mode cycling.
- [ ] NEARBY tab.

### Stage 10 — Chats (2 sessions)

- [ ] ChatsScreen list view with kind, reset cadence, unread count.
- [ ] ChatScreen with messages, bubble layout, send composer.
- [ ] Reset countdown computation.
- [ ] Nightly chat truncation job.

### Stage 11 — Polish

- [ ] Power management: deep sleep on idle, wake on touch/timer.
- [ ] Battery readout on status bar.
- [ ] Settings screen (sync interval, share mode default, font size).
- [ ] First-boot onboarding (set handle, generate keypair, etc.).

### NOT MVP (defer)

- Scribble/handwriting — needs hardware testing first, capacitive stylus
  is the only option.
- NFC sharing — requires PN532 hardware add-on.
- Sky/weather screen — designed but not yet built in simulator.
- Custom user templates — UI not yet designed.
- New folder creation UI.
- Quotes collector from books.
- Recommendation share between friends (the "Lina just liked X" feature).
- KOReader / LifeUp integration.

---

## 11. The Simulator as Living Spec

`moki-simulator.jsx` is the source of truth for UX. When in doubt:

1. Open the simulator, navigate to the screen in question, observe.
2. The state shape there is the intended state shape (camelCase → snake_case).
3. The labels there are the final labels (German, lowercase).
4. The colors and font sizes there are the intended values.

When implementing a screen, keep the simulator open in another window
and do a side-by-side check.

If the user later changes the simulator (a new feature, a redesigned
screen, a different copy), treat that as a new spec and update the
firmware to match. The simulator is faster to iterate in for design
decisions; the firmware should follow.

---

## 12. Open Questions That Need User Input

Before deep work in the relevant area, ask:

- **Sharing mechanism preference:** the user wants physical sharing
  (tap-to-share). NFC requires hardware add-on (PN532). BLE-proximity
  is software-only but less precise. Confirm before building.
- **OS / dev environment:** unknown. Affects toolchain setup steps.
- **Custom Moki theming:** the user mentioned wanting a "thema" eventually
  (forest creature / moon spirit / everyday companion). Not decided.
  The current Moki is intentionally ambiguous.
- **Real names vs handles:** the Profile shows `handle: "levin"` — confirm
  the user's intended handle on first boot.
- **First book:** simulator ships with Walden as a placeholder. Does the
  user want a different default, or to pick one at first boot?

---

## 13. Code Style Notes

- C++17 is fine, but keep usage modest. RAII for buffers, references
  where helpful, but no template gymnastics. The codebase should read
  like firmware, not like a desktop application.
- LVGL works in C; mixing with C++ is fine but use `extern "C"` where
  needed.
- Prefer fixed-size arrays over dynamic allocation for state. The 8MB
  PSRAM is plenty but heap fragmentation on long-running embedded
  devices is real. Notes bodies are the one exception (variable size).
- All user-visible strings are German, lowercase, in a single
  `strings.h` header so they can be reviewed at a glance.
- Each screen lives in its own file pair: `screens/home.cpp` + `home.h`.
  Each exports a `screen_home_create(lv_obj_t *parent)` and a
  `screen_home_update(const moki_state_t *s)`.
- State changes flow through a small event bus: components don't mutate
  state directly, they emit events (`EV_HABIT_INCREMENT`, `EV_NOTE_SAVE`,
  etc.) that the app layer handles, persists, and re-renders from.

---

## 14. Final Note

This project's success criterion is **does it feel calm to use**, not
"does it implement all the features". It is okay — preferred even — to
ship Stage 5 with Stages 6–11 stubbed out, if Stages 0–5 feel beautiful
on real hardware. The user has been disciplined about not over-stuffing
the product, and the code should respect that same restraint.

When unsure, prefer fewer features done well over more features done
adequately. The simulator is intentionally restrained; the firmware
should be too.

— end of brief —
