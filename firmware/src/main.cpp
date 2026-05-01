// ============================================================================
// MOKI · firmware · Stage 1c — epdiy + LVGL + GT911 touch
// ============================================================================
// Adds the GT911 capacitive touch driver (LILYGO SensorLib) and a screen-level
// click handler that toggles the label between "moki alive" and "moki tippt".
// Validates the full Display ↔ Touch loop on real hardware.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SD.h>
#include <limits.h>
#ifdef MOKI_WIFI
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#endif
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <epdiy.h>
#include "sdkconfig.h"
#include "lvgl.h"
#include "TouchDrvGT911.hpp"
#include "ExtensionIOXL9555.hpp"
#include "SensorPCF85063.hpp"
#include "moki_fonts.h"
#include <RadioLib.h>

// --- LoRa SX1262 (T5 S3 Pro) -------------------------------------------------
#define LORA_MISO  21
#define LORA_MOSI  13
#define LORA_SCLK  14
#define LORA_CS    46
#define LORA_IRQ   10
#define LORA_RST    1
#define LORA_BUSY  47
#define SD_CS      12

// LoRa radio is owned by variants/lilygo_t5_s3_epaper_pro/target.cpp now
// (MeshCore needs the same Module instance the BaseChatMesh stack uses).
// We expose it here under the same name (g_radio) to keep the existing
// lora_* code unchanged. RADIO_CLASS is CustomSX1262 — extends SX1262.
#include <helpers/radiolib/CustomSX1262.h>
extern CustomSX1262 radio;
#define g_radio radio
static ExtensionIOXL9555 g_xl9555;
static SensorPCF85063 g_rtc;
static bool g_rtc_ready = false;
// Last seen day-of-year (1..366). Used to detect midnight rollover for habits.
static uint16_t g_last_rollover_doy = 0;
static volatile bool     g_lora_rx_flag    = false;
static bool              g_lora_ready      = false;
static const char       *g_lora_status     = "INIT";
static uint32_t          g_lora_rx_count   = 0;
static uint32_t          g_lora_tx_count   = 0;
// Last RX diagnostics — surfaced on the chat-detail screen.
static int16_t           g_lora_last_rssi  = 0;
static float             g_lora_last_snr   = 0.0f;
static uint32_t          g_lora_last_rx_ms = 0;

// --- Moki palette (mirrors simulator exactly) -------------------------------
#define MOKI_PAPER  0xE8E2D1
#define MOKI_INK    0x1A1612
#define MOKI_DARK   0x3A342C
#define MOKI_MID    0x8A8373
#define MOKI_LIGHT  0xC9C1A8

// --- epdiy state ------------------------------------------------------------
#define WAVEFORM EPD_BUILTIN_WAVEFORM
#define DEMO_BOARD epd_board_v7

EpdiyHighlevelState hl;
int temperature = 0;

// --- GT911 touch (T5 S3 Pro pins) -------------------------------------------
#define TOUCH_SDA  39
#define TOUCH_SCL  40
#define TOUCH_IRQ  3
#define TOUCH_RST  9

static TouchDrvGT911 touch;

// ============================================================================
// Sample state (in-RAM only — Stage 3 Persistence will move this to NVS/LittleFS)
// Mirrors simulator's createInitialState (simulator.jsx lines 69-238).
// ============================================================================
typedef struct {
  char    name[48];
  uint8_t today_count;
  uint8_t streak;
  uint8_t history[84];          // 12 weeks × 7 days, count per day; idx 83 = today
} moki_habit_t;
#define HABITS_SCHEMA_V 2

// Generate plausibly varied count history (mirrors simulator's genHistory).
static void gen_habit_history(uint8_t *hist, float intensity) {
  uint32_t seed = 13;
  for (int i = 0; i < 84; i++) {
    seed = (seed * 9301u + 49297u) % 233280u;
    float r = (float)seed / 233280.0f;
    if (r > intensity) { hist[i] = 0; continue; }
    seed = (seed * 17u + 31u) % 233280u;
    float r2 = (float)seed / 233280.0f;
    if      (r2 < 0.55f) hist[i] = 1;
    else if (r2 < 0.82f) hist[i] = 2;
    else if (r2 < 0.95f) hist[i] = 3;
    else                 hist[i] = 4;
  }
}
typedef struct {
  char title[64];
  char cat[16];          // home/plants/work/self/social
  char deadline[24];     // heute/morgen/diese woche/28. apr/...
  bool recurring;
  bool done;
} moki_todo_t;
typedef struct {
  uint8_t day;
  char hour[8];
  char title[64];
  char place[48];
  char kind[12];           // private/friends/public
} moki_event_t;
#define MAX_EVENTS 16
#define EVENTS_SCHEMA_V 1

struct sample_habit_def { const char *name; uint8_t today; uint8_t streak; float intensity; };
static const sample_habit_def SAMPLE_HABITS_DEF[] = {
  { "Lesen · 10 min", 1, 4, 0.65f },
  { "Spaziergang",    2, 7, 0.85f },
  { "Wasser",         3, 2, 0.90f },
  { "Tagebuch",       0, 0, 0.30f },
};
static const int SAMPLE_HABITS_COUNT = sizeof(SAMPLE_HABITS_DEF) / sizeof(SAMPLE_HABITS_DEF[0]);

#define MAX_HABITS 16
static moki_habit_t *g_habits = nullptr;   // [MAX_HABITS] in PSRAM (heap_caps_calloc)
static int g_habits_count = 0;

static void state_init_habits(void) {
  for (int i = 0; i < SAMPLE_HABITS_COUNT && i < MAX_HABITS; i++) {
    moki_habit_t *h = &g_habits[i];
    strncpy(h->name, SAMPLE_HABITS_DEF[i].name, sizeof(h->name)-1);
    h->name[sizeof(h->name)-1] = 0;
    h->today_count = SAMPLE_HABITS_DEF[i].today;
    h->streak      = SAMPLE_HABITS_DEF[i].streak;
    gen_habit_history(h->history, SAMPLE_HABITS_DEF[i].intensity);
    h->history[83] = h->today_count;             // make today match the badge
  }
  g_habits_count = SAMPLE_HABITS_COUNT;
}

static const moki_todo_t SAMPLE_TODOS[] = {
  { "Pflanzen gießen",     "plants", "morgen",      true,  false },
  { "Lina zurückrufen",    "social", "heute",       false, false },
  { "Steuerkram sortieren","work",   "diese woche", false, false },
  { "zum arzt",            "self",   "28. apr",     false, false },
  { "Küche wischen",       "home",   "",            true,  true  },
};
static const int SAMPLE_TODOS_COUNT = sizeof(SAMPLE_TODOS) / sizeof(SAMPLE_TODOS[0]);

// Runtime todos — compose-sheet save appends here. Initialized from
// SAMPLE_TODOS in setup(). Persistence to LittleFS comes in Stage 3.
#define MAX_TODOS 32
#define TODOS_SCHEMA_V 1
static moki_todo_t *g_todos = nullptr;     // [MAX_TODOS] PSRAM-heap
static int g_todos_count = 0;

static void state_init_todos(void) {
  for (int i = 0; i < SAMPLE_TODOS_COUNT && i < MAX_TODOS; i++) {
    g_todos[i] = SAMPLE_TODOS[i];                  // struct copy
  }
  g_todos_count = SAMPLE_TODOS_COUNT;
}

// NVS-backed persistence (Arduino Preferences wrapper). Namespace = "moki".
static Preferences g_prefs;

static void state_save_todos(void) {
  g_prefs.begin("moki", false);
  g_prefs.putUInt("todos_v", TODOS_SCHEMA_V);
  g_prefs.putUInt("todos_n", (uint32_t)g_todos_count);
  if (g_todos_count > 0) {
    g_prefs.putBytes("todos", g_todos, sizeof(moki_todo_t) * g_todos_count);
  } else {
    g_prefs.remove("todos");
  }
  g_prefs.end();
  Serial.printf("[persist] saved %d todos to NVS\n", g_todos_count);
}

static void state_load_todos(void) {
  g_prefs.begin("moki", true);                     // read-only
  // todos_v: 0 = legacy (no version key, treat as v1 layout)
  //          1 = current
  uint32_t v = g_prefs.getUInt("todos_v", 0);
  uint32_t n = g_prefs.getUInt("todos_n", 0xFFFFFFFFu);
  bool ok = (n != 0xFFFFFFFFu && n <= MAX_TODOS);
  // Accept legacy (v=0) for backward-compat — same struct layout.
  if (ok && (v == 0 || v == TODOS_SCHEMA_V)) {
    g_todos_count = (int)n;
    if (g_todos_count > 0) {
      g_prefs.getBytes("todos", g_todos, sizeof(moki_todo_t) * g_todos_count);
    }
    g_prefs.end();
    Serial.printf("[persist] loaded %d todos (v=%lu)\n",
                  g_todos_count, (unsigned long)v);
    if (v == 0) state_save_todos();  // upgrade legacy in place
  } else if (ok) {
    g_prefs.end();
    Serial.printf("[persist] todos schema mismatch (got v=%lu, want %d) — keeping defaults\n",
                  (unsigned long)v, TODOS_SCHEMA_V);
    state_init_todos();
    state_save_todos();
  } else {
    g_prefs.end();
    Serial.println(F("[persist] NVS empty — bootstrapping with sample todos"));
    state_init_todos();
    state_save_todos();
  }
}

static void state_save_habits(void) {
  g_prefs.begin("moki", false);
  g_prefs.putUInt("habits_v", HABITS_SCHEMA_V);
  g_prefs.putUInt("habits_n", (uint32_t)g_habits_count);
  if (g_habits_count > 0) {
    g_prefs.putBytes("habits", g_habits, sizeof(moki_habit_t) * g_habits_count);
  } else {
    g_prefs.remove("habits");
  }
  g_prefs.end();
  Serial.printf("[persist] saved %d habits (v%u)\n", g_habits_count, HABITS_SCHEMA_V);
}

static void state_load_habits(void) {
  g_prefs.begin("moki", true);
  uint32_t v = g_prefs.getUInt("habits_v", 0);
  uint32_t n = g_prefs.getUInt("habits_n", 0xFFFFFFFFu);
  if (v == HABITS_SCHEMA_V && n != 0xFFFFFFFFu && n <= MAX_HABITS) {
    g_habits_count = (int)n;
    if (g_habits_count > 0) {
      g_prefs.getBytes("habits", g_habits, sizeof(moki_habit_t) * g_habits_count);
    }
    g_prefs.end();
    Serial.printf("[persist] loaded %d habits (v%u)\n", g_habits_count, v);
  } else {
    g_prefs.end();
    Serial.printf("[persist] habits schema mismatch (have v%u, want %u) — bootstrapping\n",
                  v, HABITS_SCHEMA_V);
    state_init_habits();
    state_save_habits();
  }
}

static const moki_event_t SAMPLE_EVENTS[] = {
  { 2, "19:00", "kochen mit lina",            "zuhause",     "private" },
  { 3, "10:00", "spaziergang philosophenweg", "heidelberg",  "friends" },
  { 5, "20:30", "lesekreis · walden",         "café frieda", "public"  },
};
static const int SAMPLE_EVENTS_COUNT = sizeof(SAMPLE_EVENTS) / sizeof(SAMPLE_EVENTS[0]);

static moki_event_t *g_events = nullptr;   // [MAX_EVENTS] PSRAM-heap
static int g_events_count = 0;

static void state_init_events(void) {
  for (int i = 0; i < SAMPLE_EVENTS_COUNT && i < MAX_EVENTS; i++) g_events[i] = SAMPLE_EVENTS[i];
  g_events_count = SAMPLE_EVENTS_COUNT;
}
static void state_save_events(void) {
  g_prefs.begin("moki", false);
  g_prefs.putUInt("events_v", EVENTS_SCHEMA_V);
  g_prefs.putUInt("events_n", (uint32_t)g_events_count);
  if (g_events_count > 0)
    g_prefs.putBytes("events", g_events, sizeof(moki_event_t) * g_events_count);
  else
    g_prefs.remove("events");
  g_prefs.end();
  Serial.printf("[persist] saved %d events\n", g_events_count);
}
static void state_load_events(void) {
  g_prefs.begin("moki", true);
  uint32_t v = g_prefs.getUInt("events_v", 0);
  uint32_t n = g_prefs.getUInt("events_n", 0xFFFFFFFFu);
  bool ok = (n != 0xFFFFFFFFu && n <= MAX_EVENTS);
  if (ok && (v == 0 || v == EVENTS_SCHEMA_V)) {
    g_events_count = (int)n;
    if (g_events_count > 0)
      g_prefs.getBytes("events", g_events, sizeof(moki_event_t) * g_events_count);
    g_prefs.end();
    Serial.printf("[persist] loaded %d events (v=%lu)\n",
                  g_events_count, (unsigned long)v);
    if (v == 0) state_save_events();
  } else if (ok) {
    g_prefs.end();
    Serial.printf("[persist] events schema mismatch (v=%lu, want %d) — defaults\n",
                  (unsigned long)v, EVENTS_SCHEMA_V);
    state_init_events();
    state_save_events();
  } else {
    g_prefs.end();
    state_init_events();
    state_save_events();
  }
}

// Chats — direct/group/public with optional reset cadence
typedef struct {
  const char *kind;     // direct/group/public
  const char *name;
  const char *last;
  const char *ts;
  uint8_t     unread;
  const char *reset;    // ""/daily/weekly
} moki_chat_t;
static const moki_chat_t SAMPLE_CHATS[] = {
  { "lora",   "#moki-mesh",    "...",                            "live",       0, ""       },
  { "direct", "lina",          "magst du samstag tanzen?",       "vor 8 min",  1, ""       },
  { "group",  "lesekreis",     "walden kap 4 bis freitag ok?",   "vor 2h",     0, "weekly" },
  { "public", "#rhein-neckar", "jemand heute abend am neckar?",  "vor 45 min", 3, "daily"  },
  { "public", "#bücher",       "— neu —",                        "still",      0, "weekly" },
};
static const int SAMPLE_CHATS_COUNT = sizeof(SAMPLE_CHATS) / sizeof(SAMPLE_CHATS[0]);

// Nearby peers (for MAP "in der nähe" tab)
typedef struct {
  const char *name;
  const char *dist;
  const char *mood;       // simulator mood id (we just show label)
  const char *last_heard;
} moki_nearby_t;
static const moki_nearby_t SAMPLE_NEARBY[] = {
  { "lina", "~80 m",  "bierchen",    "im sync vor 3 min"  },
  { "tom",  "~200 m", "spazieren",   "im sync vor 12 min" },
  { "juno", "~450 m", "brettspiele", "im sync vor 2h"     },
};
static const int SAMPLE_NEARBY_COUNT = sizeof(SAMPLE_NEARBY) / sizeof(SAMPLE_NEARBY[0]);

// Map places and friends-live (for MAP "karte" tab) — coords are 0..100 % of
// the map area, mirroring the simulator's normalized coordinate system.
typedef struct { float x; float y; const char *name; const char *kind; } moki_place_t;
static const moki_place_t SAMPLE_PLACES[] = {
  { 38, 42, "café frieda",     "saved" },
  { 62, 30, "zuhause",         "home"  },
  { 72, 58, "philosophenweg",  "saved" },
  { 28, 68, "schwimmbad",      "saved" },
};
static const int SAMPLE_PLACES_COUNT = sizeof(SAMPLE_PLACES) / sizeof(SAMPLE_PLACES[0]);

typedef struct { float x; float y; const char *name; const char *fresh; } moki_friend_live_t;
static const moki_friend_live_t SAMPLE_FRIENDS_LIVE[] = {
  { 44, 46, "lina", "3 min"  },
  { 70, 62, "tom",  "12 min" },
};
static const int SAMPLE_FRIENDS_LIVE_COUNT =
  sizeof(SAMPLE_FRIENDS_LIVE) / sizeof(SAMPLE_FRIENDS_LIVE[0]);

// Mood presets (mirror simulator.jsx lines 36-45)
typedef struct { const char *id; const char *icon; const char *label; const char *hint; } moki_mood_def_t;
static const moki_mood_def_t MOOD_PRESETS[] = {
  { "camp",  "⛺", "campen",       "mal wieder raus"  },
  { "sport", "⚡", "sport",        "tischtennis · rad"},
  { "food",  "◒", "essen gehen",  "wer mag mit"      },
  { "drink", "◉", "bierchen",     "einfach quatschen"},
  { "spont", "✦", "spontan",      "alles möglich"    },
  { "games", "◈", "brettspiele",  "heute abend?"     },
  { "walk",  "◐", "spazieren",    "mit gespräch"     },
  { "read",  "☾", "still lesen",  "im café, gemeinsam"},
};
static const int MOOD_COUNT = sizeof(MOOD_PRESETS) / sizeof(MOOD_PRESETS[0]);

static char g_active_mood[16] = "";  // empty = no mood shared

// ----------------------------------------------------------------------------
// Notes (Stage 6) — stored in NVS as a single blob with a fixed layout.
// Body is inline 1024 bytes — sufficient for short journal entries; very long
// notes get truncated. LittleFS migration is a future polish step.
// ----------------------------------------------------------------------------
typedef struct {
  char     title[64];
  char     body[1024];
  char     templ[16];          // blank/diary/recipe/list/idea
  char     folder[24];         // tagebuch/küche/gelesen/ideen/""
  char     visibility[16];     // private/friends/public
  bool     pinned;
  uint32_t updated_at;         // millis() at last edit (relative; resets on boot)
} moki_note_t;

#define MAX_NOTES 16
#define NOTES_SCHEMA_V 1

// ============================================================================
// M5-Lite — Pageable Book Reader
// ============================================================================
// One embedded sample (Walden chapter excerpt) + char-based pagination.
// Real EPUB parser will land in M5-Full when we wire up microSD.
//
// Pagination is character-based (CHARS_PER_PAGE) with word-boundary trimming
// so we don't split mid-word. ~1400 chars fits comfortably on the 4.7"
// screen with our Fraunces 22pt italic body.
#define BOOK_CHARS_PER_PAGE 1400
static const char MOKI_BOOK_TEXT[] PROGMEM =
  "Ich ging in die Wälder, weil ich mit Bedacht leben wollte, "
  "um nur den wesentlichen Tatsachen des Lebens ins Auge zu sehen, "
  "und zu lernen, was es zu lehren hatte — und nicht, wenn es zum "
  "Sterben käme, zu entdecken, dass ich nicht gelebt hatte.\n\n"

  "Ich wollte nicht das leben, was nicht Leben war; das Leben ist so "
  "kostbar; auch wollte ich nicht entsagen, es sei denn, dass es "
  "durchaus nötig wäre. Ich wollte tief leben und alles Mark des "
  "Lebens aussaugen, so kraftvoll und spartanisch leben, dass alles, "
  "was nicht Leben war, in die Flucht geschlagen würde, eine Schneise "
  "schlagen und kurz halten, das Leben in eine Ecke treiben und "
  "auf seine niedrigsten Begriffe reduzieren.\n\n"

  "Wenn es sich als gemein erweisen sollte, dann eben das ganze und "
  "echte Gemeine herauszufinden und seine Gemeinheit der Welt zu "
  "verkünden; oder, wenn es erhaben wäre, es aus eigener Erfahrung zu "
  "wissen und einen wahren Bericht davon in meinem nächsten Ausflug "
  "geben zu können.\n\n"

  "Denn die meisten Menschen, scheint es mir, sind in einem seltsamen "
  "Zweifel über das Leben, ob es vom Teufel oder von Gott ist, und "
  "haben etwas vorschnell gefolgert, dass es das Hauptziel des "
  "Menschen hier sei, 'Gott zu verherrlichen und sich seiner für "
  "immer zu erfreuen'.\n\n"

  "Trotzdem leben wir gemein, wie Ameisen; obwohl die Sage sagt, "
  "wir wären schon vor langer Zeit in Menschen verwandelt worden; "
  "wie Pygmäen kämpfen wir mit Kranichen; es ist Fehler über Fehler, "
  "und Flicken über Flicken, und unsere beste Tugend hat einen "
  "Anlass aus überflüssigem und vermeidbarem Elend.\n\n"

  "Unser Leben wird in Kleinigkeiten zerstreut. Ein ehrlicher Mann "
  "hat kaum nötig, mehr als seine zehn Finger zu zählen, oder in "
  "extremen Fällen kann er seine zehn Zehen hinzufügen und den Rest "
  "in einer Pauschale zusammenwerfen. Einfachheit, Einfachheit, "
  "Einfachheit! Ich sage, lass deine Geschäfte zwei oder drei sein, "
  "nicht hundert oder tausend; statt einer Million zähle ein halbes "
  "Dutzend, und führe deine Rechnung auf deinem Daumennagel.\n\n"

  "Inmitten dieses bewegten und stürmischen und doch nervösen "
  "neunzehnten Jahrhunderts, in dem wir leben, gibt es niemals "
  "weniger als drei Mahlzeiten am Tag, und sie sind absolut "
  "notwendig; und wenn die Verdauung schlecht ist, fehlt uns dann "
  "immer noch der Appetit darauf? Statt drei Mahlzeiten am Tag, "
  "wenn nötig, iss nur eine; statt hundert Gerichte, fünf; und "
  "reduziere andere Dinge im selben Verhältnis.\n\n"

  "Unser Leben ist wie eine Deutsche Konföderation, deren "
  "Bestandteile aus winzigen, sich ständig verändernden Staaten "
  "bestehen, deren Grenzen kein einziger Deutscher von Augenblick "
  "zu Augenblick aufzeigen kann. Die Nation selbst, mit all ihren "
  "sogenannten inneren Verbesserungen, die übrigens äußerlich und "
  "oberflächlich sind, ist genau eine solche unhandliche und "
  "übermäßig gewachsene Einrichtung.";
#define BOOK_PAGE_KEY "book_p"
static int g_book_page = 0;

// Active book selector:
//   -2 = embedded MOKI_MANUAL_TEXT (Bedienungsanleitung)
//   -1 = embedded MOKI_BOOK_TEXT (Walden)
//    0..N = index into g_books[] (SD-card .txt files)
static int g_book_active_idx = -2;   // default to manual on first boot

// Bedienungsanleitung — embedded so it's always available even without SD.
static const char MOKI_MANUAL_TEXT[] PROGMEM =
  "willkommen.\n\n"
  "moki ist dein kleiner companion. anti-smartphone. e-ink, leise, "
  "langsam. nur das nötigste, schön gemacht. dieser kurze leitfaden "
  "zeigt dir wie alles zusammenhängt.\n\n"

  "das gehäuse.\n\n"
  "vorderseite: e-ink display, kapazitiver touch (ein finger). "
  "rechte seite: usb-c zum laden. unten: micro-sd slot für bücher und "
  "karten. die antenne ragt nach oben heraus — niemals senden ohne sie, "
  "der lora-chip nimmt sonst schaden.\n\n"

  "navigation.\n\n"
  "unten am rand siehst du fünf reiter: heim, tun, lesen, chat, karte. "
  "tap, um zu wechseln. der aktive reiter hat eine 1.5px linie unter dem "
  "text.\n\n"

  "heim.\n\n"
  "der startbildschirm. moki begrüsst dich, du siehst statistiken zu "
  "todos, gewohnheiten und tag. tap auf eine kachel öffnet die jeweilige "
  "ansicht. unten gibt es eine stimmungs-anzeige (mood) — tap, um deine "
  "stimmung zu setzen.\n\n"

  "tun.\n\n"
  "drei tabs: todos, gewohnheiten, kalender. todos sind einfache "
  "aufgaben mit kategorie und deadline. gewohnheiten haben einen + "
  "und – button und eine 12-wochen heatmap. tag wechselt automatisch um "
  "mitternacht. kalender zeigt deine termine.\n\n"

  "lesen.\n\n"
  "drei tabs: buch, feed, notizen. buch ist genau das, was du gerade "
  "liest. lege .txt-dateien in /books/ auf der sd-karte ab — moki "
  "indexiert sie beim booten und zeigt sie als chips oben. dein "
  "lesefortschritt wird pro buch gespeichert.\n\n"

  "notizen sind kurze markdown-texte. tap +, um eine neue zu beginnen. "
  "drei vorlagen: tagebuch, sachen, anders. unterstützte syntax: "
  "# überschrift, ## zwischenüberschrift, - listenpunkt, > zitat, --- "
  "trennlinie.\n\n"

  "chat.\n\n"
  "moki kommuniziert per lora — ein langsamer funkstandard, der "
  "kilometer überbrückt aber nur ein paar nachrichten pro minute "
  "schafft. das ist absicht: keine flut, keine push-tyrannei.\n\n"

  "der chat reiter zeigt deine räume und deine moki-freunde. ein raum "
  "ist ein verschlüsselter kanal, an dem mehrere mokis teilnehmen "
  "können. tap auf einen raum öffnet ihn — du siehst die letzten "
  "nachrichten und kannst eine eigene tippen.\n\n"

  "moki-freunde sind andere mokis, die du im mesh entdeckt hast. tap "
  "auf einen namen öffnet eine direktnachricht — nur ihr beide könnt "
  "sie lesen. wenn du selbst entdeckt werden willst, gehe ins profil "
  "und drücke 'mich im mesh zeigen'.\n\n"

  "antenne.\n\n"
  "die lora-antenne muss angeschraubt sein, bevor du tx-fähig bist. "
  "ohne antenne darf moki nur empfangen — das verhindert chip-schäden. "
  "in den einstellungen findest du den schalter 'antenne dran'. erst "
  "dann kannst du senden.\n\n"

  "ein hinweis zur reichweite: ohne hindernisse erreicht moki etwa zwei "
  "kilometer auf 869 mhz. mit den repeatern des rhein-neckar-mesh "
  "decken deine nachrichten ganz heidelberg ab — auch wenn dein freund "
  "auf der anderen seite des neckars sitzt. die nachricht hopst über "
  "mehrere repeater, völlig automatisch.\n\n"

  "karte.\n\n"
  "moki kennt grob seine umgebung. wenn die gps-antenne aktiv ist (und "
  "du draussen bist), siehst du deine eigene position in der mitte und "
  "in der nähe befindliche freunde, sofern sie ihre position teilen. "
  "die karte ist bewusst minimalistisch — keine bunten labels, nur "
  "wesentliches.\n\n"

  "einstellungen.\n\n"
  "im profil-screen findest du den eingang zu den einstellungen. dort "
  "regelst du:\n\n"

  "sync-intervall: wie oft moki versucht, sich mit dem mesh zu syncen. "
  "kürzer = aktueller, länger = mehr akku.\n\n"

  "auto-schlaf: nach wieviel minuten leerlauf moki in den tiefschlaf "
  "geht. tap aufs display weckt ihn wieder. 1 min = sehr sparsam, aus "
  "= immer ansprechbar aber teurer akku.\n\n"

  "antenne dran: tx-freigabe wie oben beschrieben.\n\n"

  "akku.\n\n"
  "ein voll geladener akku reicht je nach einstellung zwischen einer "
  "woche (kurze sync-intervalle, kein auto-schlaf) und vier wochen "
  "(maximale sparsamkeit). usb-c lädt in zwei stunden voll.\n\n"

  "datenschutz.\n\n"
  "alle mesh-nachrichten sind ende-zu-ende verschlüsselt mit aes-128. "
  "deine identität ist ein lokal generiertes ed25519-schlüsselpaar — "
  "nie auf irgendeinen server hochgeladen. du kannst eigene private "
  "kanäle mit einem geheimen psk anlegen, dann sieht euch niemand "
  "anders.\n\n"

  "tipps.\n\n"
  "moki ist designed für kurze interaktion. du sollst kein konzept von "
  "sucht entwickeln. ein paar minuten todos checken, ein paar seiten "
  "lesen, eine notiz tippen, eine nachricht schreiben — dann zur seite "
  "legen.\n\n"

  "wenn etwas nicht funktioniert, hilft fast immer ein neustart. der "
  "reset-button ist auf der seite versteckt. todos, notizen, freunde "
  "und einstellungen überleben einen reboot.\n\n"

  "viel freude.";
static moki_note_t *g_notes = nullptr;     // [MAX_NOTES] PSRAM-heap (~18KB)
static int g_notes_count = 0;

typedef struct { const char *id; const char *label; const char *body; } moki_note_template_t;
static const moki_note_template_t NOTE_TEMPLATES[] = {
  { "blank",  "leer",     "" },
  { "diary",  "tagebuch", "# [datum]\n\n## war gut\n- \n\n## gelernt\n- \n\n## dankbar\n- " },
  { "recipe", "rezept",   "# [name]\n\n*für [personen]*\n\n## zutaten\n- \n\n## zubereitung\n1. \n\n## notiz\n- " },
  { "list",   "liste",    "# [titel]\n\n- \n- \n- " },
  { "idea",   "idee",     "# [idee]\n\n> gedanke\n\n## warum\n\n## nächste schritte\n- " },
};
static const int NOTE_TEMPLATE_COUNT = sizeof(NOTE_TEMPLATES) / sizeof(NOTE_TEMPLATES[0]);

static void state_init_notes(void) {
  // Sample content — Walden gedanken + miso-ramen + dienstag tagebuch
  static const moki_note_t SAMPLE[] = {
    { "Walden · Gedanken",
      "# Walden\n\n> „Die meisten Menschen führen ein Leben stiller Verzweiflung.\"\n\n## was bleibt hängen\n- einfachheit als politische handlung\n- der **wald** als spiegel, nicht als flucht\n\n## nächste kapitel\n- kap. 4 bis freitag",
      "idea", "gelesen", "private", true, 0 },
    { "Miso-Ramen · einfach",
      "# miso-ramen\n\n*abends, für 2 personen*\n\n## zutaten\n- 400g ramen-nudeln\n- 3 el weißes miso\n- 1 l brühe\n- 2 lauchzwiebeln\n- 1 ei pro person\n\n## zubereitung\n1. eier 7 min kochen, kalt abschrecken\n2. brühe erhitzen, miso einrühren (**nicht kochen**)\n3. nudeln separat garen\n4. alles in schalen schichten",
      "recipe", "küche", "friends", false, 0 },
    { "dienstag",
      "# dienstag · 20. april\n\n## war gut\n- lange am neckar gesessen\n- lina hat gelacht\n\n## gelernt\n- ich kann langsamer lesen\n\n## dankbar\n- für den kaffee heute morgen",
      "diary", "tagebuch", "private", false, 0 },
  };
  int n = (int)(sizeof(SAMPLE) / sizeof(SAMPLE[0]));
  for (int i = 0; i < n && i < MAX_NOTES; i++) g_notes[i] = SAMPLE[i];
  g_notes_count = n;
}

static const char *chat_kind_glyph(const char *kind) {
  if (!strcmp(kind,"direct")) return "1:1";
  if (!strcmp(kind,"group"))  return "GR";
  if (!strcmp(kind,"lora"))   return "LORA";
  return "PUB";
}
static const char *chat_reset_phrase(const char *reset) {
  if (!strcmp(reset,"daily"))  return "noch heute · dann leer";
  if (!strcmp(reset,"weekly")) return "noch wenige tage · dann leer";
  return "";
}

// Category visuals (from simulator)
static const char *cat_label(const char *cat) {
  if (!strcmp(cat,"home"))   return "zuhause";
  if (!strcmp(cat,"plants")) return "pflanzen";
  if (!strcmp(cat,"work"))   return "arbeit";
  if (!strcmp(cat,"self"))   return "selbst";
  if (!strcmp(cat,"social")) return "freund_innen";
  return cat;
}
static const char *cat_mark(const char *cat) {
  // Cat-marks need to live in our font glyph set (ASCII + selected unicode).
  // Use single-letter abbreviations where geometric-shapes glyphs are absent.
  if (!strcmp(cat,"home"))   return "H";
  if (!strcmp(cat,"plants")) return "P";
  if (!strcmp(cat,"work"))   return "W";
  if (!strcmp(cat,"self"))   return "S";
  if (!strcmp(cat,"social")) return "F";
  return "·";
}

// ============================================================================
// Screen IDs + nav
// ============================================================================
typedef enum { SCR_HOME = 0, SCR_DO, SCR_READ, SCR_CHAT, SCR_MAP,
               SCR_MOOD, SCR_PROFILE,
               SCR_NOTE_NEW, SCR_NOTE_EDIT,
               SCR_CHAT_DETAIL,
               SCR_READER,
               SCR_PIN_DETAIL,
               SCR_PIN_RENAME,
               SCR_PIN_NOTE,
               SCR_WIFI,
               SCR_WIFI_PSK,
               SCR_OTA,
               SCR_SETTINGS } screen_id_t;

// LoRa preset IDs — mirrored in lora_apply_preset()
enum {
  LORA_PRESET_MOKI            = 0,  // 868.0/BW250/SF10/CR6/sync 0xAB — private Moki-Welle
  LORA_PRESET_MESHCORE_NARROW = 1,  // 869.618/BW62.5/SF8/CR8/sync 0x12 — RN-Mesh (Standard seit Oct 2025)
  LORA_PRESET_MESHCORE_LEGACY = 2,  // 869.525/BW250/SF11/CR5/sync 0x12 — MeshCore pre-2025
  LORA_PRESET_MESHTASTIC_LF   = 3,  // 869.525/BW250/SF11/CR5/sync 0x2B — andere HD-User auf Meshtastic
};
#define LORA_PRESET_COUNT 4
static const char *LORA_PRESET_LABELS[LORA_PRESET_COUNT] = {
  "moki", "narrow", "legacy", "mtast"
};

// Settings (persistent). Order matters: new fields APPENDED, not inserted, so
// older NVS blobs still load correctly via partial-match in state_load_settings.
// SCHEMA POLICY:
//   - v1: initial layout (sync_interval through lora_tx_armed)
//   - v2: + lora_preset (uint8) appended
//   - v3: + mesh_channel_name + mesh_channel_psk_b64 (MeshCore channel)
//   - v4: + auto_sleep_min, mesh_active_channel (multi-channel selector)
//   Bump SETTINGS_SCHEMA_V whenever a field is reordered/removed/changed type.
//   Pure appends keep the version (size-based partial-load handles them).
#define SETTINGS_SCHEMA_V 4
typedef struct {
  uint8_t  sync_interval_min;     // 5 / 15 / 30 / 60
  char     share_default[8];      // off / hourly / live
  char     handle[32];            // user handle
  char     bio[80];
  bool     lora_tx_armed;         // false = RX-only (safe without antenna)
  uint8_t  lora_preset;           // LORA_PRESET_*
  char     mesh_channel_name[16]; // First channel label (legacy, kept for compat)
  char     mesh_channel_psk_b64[28]; // First channel PSK (legacy)
  uint8_t  auto_sleep_min;        // 0 = off, otherwise sleep after N min idle
  uint8_t  mesh_active_channel;   // Index into g_channels[] for TX target
} moki_settings_t;

// ── Multi-Channel Support (Phase 1 of M4) ─────────────────────────────────
// Up to 4 channels can be active at once. Each has a name + Base64 PSK.
// Channel #0 mirrors mesh_channel_* fields above for legacy migration.
// Channels persist as a separate NVS blob "channels" with schema version.
#define MAX_MOKI_CHANNELS 4
#define CHANNELS_SCHEMA_V 1
typedef struct {
  char name[16];
  char psk_b64[28];
  bool active;       // true = registered with MeshCore at boot
} moki_channel_t;
static moki_channel_t g_channels[MAX_MOKI_CHANNELS];
static int g_num_channels = 0;
// Default uses MeshCore's well-known public PSK so out-of-the-box two Mokis
// can talk to each other (and any other MeshCore-Public participant).
static moki_settings_t g_settings = {
  30, "hourly", "levin", "liest langsam. läuft lieber.", false,
  LORA_PRESET_MESHCORE_NARROW,
  "moki",
  "izOH6cXN6mrJ5e26oRXNcg==",
  0,   // auto_sleep_min — disabled by default (Lucas opts in)
  0    // mesh_active_channel — index 0 (first channel)
};

// Activity tracker for auto-sleep — touched on any user input.
static uint32_t g_last_activity_ms = 0;
static inline void mark_activity(void) { g_last_activity_ms = millis(); }

// LoRa-Chat ring buffer of recent messages.
// Body is stored as raw bytes (may contain non-printable / non-NUL-terminated).
//
// channel_idx tagging:
//    0..3   — MeshCore channel index (matches g_channels[idx])
//    -1     — local sent (no remote channel — TX echo)
//    -2     — foreign-protocol RX (Meshtastic, raw, etc.)
typedef struct {
  uint32_t ts_ms;
  int16_t  rssi;
  int8_t   snr_x10;   // SNR * 10 — e.g. -75 = -7.5 dB
  uint8_t  len;       // raw byte length (≤ sizeof(text))
  uint8_t  preset;    // LORA_PRESET_* active when received
  int8_t   channel_idx; // see comment above
  char     from[24];
  char     text[160]; // raw payload bytes (NOT NUL-terminated guaranteed)
} moki_lora_msg_t;
#define LORA_MSG_CAP 32
static moki_lora_msg_t *g_lora_msgs = nullptr;   // [LORA_MSG_CAP] PSRAM-heap
static int g_lora_msg_count = 0;          // total received (for ring index)
static int g_lora_msg_head  = 0;          // ring buffer head

// Forward decl — actual definition lives in the LittleFS section below.
static void lora_persist_append(const moki_lora_msg_t *m);

// Bridge for moki_mesh.cpp — exposes lora_push_msg via C-linkage so the
// MyMesh subclass can dump received channel-messages into our existing
// ring buffer + UI.
extern "C" void moki_lora_push_msg_external(const char *from, const char *text, int16_t rssi) {
  // Backward-compat shim — defaults to "active channel" tagging.
  moki_lora_msg_t *m = &g_lora_msgs[g_lora_msg_head];
  strncpy(m->from, from, sizeof(m->from)-1); m->from[sizeof(m->from)-1] = 0;
  size_t tlen = strlen(text);
  if (tlen > sizeof(m->text)) tlen = sizeof(m->text);
  memcpy(m->text, text, tlen);
  m->len    = (uint8_t)tlen;
  m->rssi   = rssi;
  m->snr_x10 = 0;
  m->preset  = g_settings.lora_preset;
  m->channel_idx = (int8_t)g_settings.mesh_active_channel;
  m->ts_ms  = millis();
  g_lora_msg_head = (g_lora_msg_head + 1) % LORA_MSG_CAP;
  if (g_lora_msg_count < LORA_MSG_CAP) g_lora_msg_count++;
  lora_persist_append(m);
  g_lora_rx_count++;
}

// Channel-tagged variant — moki_mesh.cpp uses this so each MeshCore
// channel's messages get filed into the correct tab.
extern "C" void moki_lora_push_msg_channel(const char *from, const char *text,
                                            int16_t rssi, int channel_idx) {
  moki_lora_msg_t *m = &g_lora_msgs[g_lora_msg_head];
  strncpy(m->from, from, sizeof(m->from)-1); m->from[sizeof(m->from)-1] = 0;
  size_t tlen = strlen(text);
  if (tlen > sizeof(m->text)) tlen = sizeof(m->text);
  memcpy(m->text, text, tlen);
  m->len    = (uint8_t)tlen;
  m->rssi   = rssi;
  m->snr_x10 = 0;
  m->preset  = g_settings.lora_preset;
  m->channel_idx = (int8_t)channel_idx;
  m->ts_ms  = millis();
  g_lora_msg_head = (g_lora_msg_head + 1) % LORA_MSG_CAP;
  if (g_lora_msg_count < LORA_MSG_CAP) g_lora_msg_count++;
  lora_persist_append(m);
  g_lora_rx_count++;
}

// MeshCore C-API forward decls (defined in moki_mesh.cpp).
extern "C" {
  void moki_mesh_init(void);
  void moki_mesh_loop(void);
  bool moki_mesh_send(const char *sender_name, const char *text);
  bool moki_mesh_advert(const char *name);
  bool moki_mesh_advert_with_loc(const char *name, double lat, double lon);
  int  moki_mesh_contact_count();
  bool moki_mesh_get_contact(int idx, char *out_name, int name_size, uint8_t out_pubkey4[4]);
  bool moki_mesh_get_contact_loc(int idx, float *out_lat, float *out_lon,
                                 uint32_t *out_last_advert_ts);
  bool moki_mesh_dm(int contact_idx, const char *text);
}

// Settings accessors for moki_mesh.cpp (so it doesn't need the full struct).
extern "C" const char *moki_settings_get_channel_name() {
  return g_settings.mesh_channel_name;
}
extern "C" const char *moki_settings_get_channel_psk() {
  return g_settings.mesh_channel_psk_b64;
}

// Multi-channel API: iterate all configured channels at boot, query active.
extern "C" int moki_channels_count() { return g_num_channels; }
extern "C" const char *moki_channel_name(int idx) {
  if (idx < 0 || idx >= g_num_channels) return NULL;
  return g_channels[idx].name;
}
extern "C" const char *moki_channel_psk(int idx) {
  if (idx < 0 || idx >= g_num_channels) return NULL;
  return g_channels[idx].psk_b64;
}
extern "C" int moki_channels_active_idx() {
  return (g_settings.mesh_active_channel < g_num_channels)
         ? g_settings.mesh_active_channel : 0;
}

static void lora_push_msg(const char *from, const char *text, int16_t rssi) {
  moki_lora_msg_t *m = &g_lora_msgs[g_lora_msg_head];
  strncpy(m->from, from, sizeof(m->from)-1); m->from[sizeof(m->from)-1] = 0;
  size_t tlen = strlen(text);
  if (tlen > sizeof(m->text)) tlen = sizeof(m->text);
  memcpy(m->text, text, tlen);
  m->len    = (uint8_t)tlen;
  m->rssi   = rssi;
  m->snr_x10 = 0;
  m->preset  = g_settings.lora_preset;
  // Local TX echo — tag with currently-active channel so it appears in the
  // right tab. Direct messages (DM) override later.
  m->channel_idx = (rssi == 0)
                   ? (int8_t)g_settings.mesh_active_channel
                   : -2;   // raw RX with rssi → foreign-protocol bucket
  m->ts_ms  = millis();
  g_lora_msg_head = (g_lora_msg_head + 1) % LORA_MSG_CAP;
  if (g_lora_msg_count < LORA_MSG_CAP) g_lora_msg_count++;
  lora_persist_append(m);
}

// ── LittleFS-backed persistence for LoRa-Chat ─────────────────────────────
// Stores raw moki_lora_msg_t records in append-only log /lora.log.
// Survives reboots & battery-pulls (vs RAM ring buffer alone).
//
// Schema-Version 1 — sizeof(moki_lora_msg_t) is the record size.
// If the struct layout changes, bump LORA_LOG_SCHEMA_V and add migration.
#define LORA_LOG_PATH "/lora.log"
#define LORA_LOG_SCHEMA_V 1
#define LORA_LOG_MAX_BYTES (256u * 1024u)  // ~1300 messages → soft cap, then truncate

static bool g_fs_ready = false;
static bool g_sd_ready = false;

// ── GPS state (M6 step 1) ────────────────────────────────────────────────
// Serial2 @ GPS_BAUD_RATE (typ. 9600) auf PIN_GPS_RX/PIN_GPS_TX. NMEA aus
// dem MIA-M10Q oder L76K Modul. Wir parsen $G[NP]GGA + $G[NP]RMC für
// (lat, lon, fix, sats, hdop). Update-Rate ~1Hz vom Modul her.
typedef struct {
  bool     fix;             // hat Modul gerade einen 2D/3D-Fix?
  float    lat_deg;         // dezimal, North positiv
  float    lon_deg;         // dezimal, East positiv
  uint8_t  sats_used;       // Sats im Fix (aus GGA)
  uint8_t  sats_tracked;    // Sats mit SNR>0 (aus GSV) — "ich sehe was"
  uint8_t  hdop_x10;        // HDOP × 10 (z.B. 27 = 2.7)
  uint32_t fix_ts_ms;       // millis() beim letzten Fix
  bool     antenna_ok;      // GPTXT,ANTENNA OK gesehen
} moki_gps_t;
static moki_gps_t g_gps = { false, 0.0f, 0.0f, 0, 0, 0, 0, false };
// Per-constellation tracked-sats counters (rebuilt during a GSV burst).
// Modul sendet GP/GL/GA Blöcke je in mehreren Zeilen. Wir summieren über
// alle Talker (GP, GL, GA) während eines Sweeps und kopieren bei nächstem
// GGA in g_gps.sats_tracked.
static uint8_t g_gsv_running = 0;
static HardwareSerial GPSSerial(2);
static char  g_nmea_buf[96];
static size_t g_nmea_len = 0;

// Convert NMEA "ddmm.mmmm" + "N/S" or "dddmm.mmmm" + "E/W" → decimal degrees
static float nmea_to_decimal(const char *raw, char hemi, bool is_lon) {
  if (!raw || !*raw) return 0.0f;
  float val = atof(raw);
  int   deg = is_lon ? (int)(val / 100.0f) : (int)(val / 100.0f);
  float min = val - (float)deg * 100.0f;
  float dec = (float)deg + min / 60.0f;
  if (hemi == 'S' || hemi == 'W') dec = -dec;
  return dec;
}

// Split a comma-separated NMEA sentence into fields (in-place; mutates buf).
// Returns count, fills `fields[i]` with pointer to start of i-th field.
static int nmea_split(char *buf, char **fields, int max_fields) {
  int n = 0;
  fields[n++] = buf;
  for (char *p = buf; *p && n < max_fields; p++) {
    if (*p == ',') { *p = 0; fields[n++] = p + 1; }
    else if (*p == '*') { *p = 0; break; }
  }
  return n;
}

// Parse one complete NMEA line (without trailing CR/LF). Updates g_gps.
// Forward — defined later near map renderer
static void self_pos_update_from_gps(void);

static void nmea_parse_line(char *line, size_t len) {
  if (len < 7 || line[0] != '$') return;
  // Talker-ID + sentence: $GxGGA, $GxRMC etc. We accept any talker (GP, GN, GL).
  const char *type = line + 3;
  char *fields[20] = {0};
  int   nf = nmea_split(line, fields, 20);

  if (memcmp(type, "GGA", 3) == 0 && nf >= 9) {
    // $..GGA,hhmmss.ss,lat,N,lon,E,fix_quality,sats,hdop,...
    // GGA kommt einmal pro Sekunde — guter Moment, GSV-Snapshot zu übernehmen.
    int fix_q = atoi(fields[6]);
    g_gps.sats_used = (uint8_t)atoi(fields[7]);
    g_gps.sats_tracked = g_gsv_running;
    g_gsv_running = 0;
    float hdop = atof(fields[8]);
    g_gps.hdop_x10 = (uint8_t)(hdop * 10.0f);
    if (fix_q > 0 && fields[2][0] && fields[4][0]) {
      g_gps.lat_deg = nmea_to_decimal(fields[2], fields[3][0], false);
      g_gps.lon_deg = nmea_to_decimal(fields[4], fields[5][0], true);
      g_gps.fix     = true;
      g_gps.fix_ts_ms = millis();
      self_pos_update_from_gps();
    }
  } else if (memcmp(type, "RMC", 3) == 0 && nf >= 7) {
    // $..RMC,hhmmss.ss,status,lat,N,lon,E,...
    if (fields[2][0] == 'A' && fields[3][0] && fields[5][0]) {
      g_gps.lat_deg = nmea_to_decimal(fields[3], fields[4][0], false);
      g_gps.lon_deg = nmea_to_decimal(fields[5], fields[6][0], true);
      g_gps.fix     = true;
      g_gps.fix_ts_ms = millis();
      self_pos_update_from_gps();
    } else if (fields[2][0] == 'V') {
      g_gps.fix = false;
    }
  } else if (memcmp(type, "GSV", 3) == 0 && nf >= 4) {
    // $xxGSV,total_msgs,msg_num,sats_in_view,[prn,el,az,snr]*4,...
    // Pro Block 4 Sats. SNR ist letztes Feld jedes Sat-Quadrupels. Wir zählen
    // alle mit nicht-leerem SNR (also tatsächlich getrackt, nicht nur gesehen).
    for (int i = 4; i + 3 < nf; i += 4) {
      const char *snr = fields[i + 3];
      if (snr && snr[0] && snr[0] != '*') g_gsv_running++;
    }
  } else if (memcmp(type, "TXT", 3) == 0 && nf >= 5) {
    // $GPTXT,01,01,01,ANTENNA OK
    if (strstr(fields[4], "ANTENNA OK")) g_gps.antenna_ok = true;
  }
}

static void gps_init(void) {
  GPSSerial.begin(GPS_BAUD_RATE, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  Serial.printf("[gps] Serial2 begin @ %d (RX=%d, TX=%d)\n",
                GPS_BAUD_RATE, PIN_GPS_RX, PIN_GPS_TX);
}

static void gps_loop(void) {
  while (GPSSerial.available()) {
    int c = GPSSerial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      g_nmea_buf[g_nmea_len] = 0;
      if (g_nmea_len > 6) nmea_parse_line(g_nmea_buf, g_nmea_len);
      g_nmea_len = 0;
      continue;
    }
    if (g_nmea_len < sizeof(g_nmea_buf) - 1) {
      g_nmea_buf[g_nmea_len++] = (char)c;
    } else {
      // Overlong line (probably garbage) — reset
      g_nmea_len = 0;
    }
  }
  // Stale-fix decay: if no fresh data for 10s, mark unfixed
  if (g_gps.fix && (millis() - g_gps.fix_ts_ms) > 10000) {
    g_gps.fix = false;
  }
}

#ifdef MOKI_WIFI
// ── WiFi (M8.2) ──────────────────────────────────────────────────────────
// STA-Mode für OTA-Updates. Credentials in NVS — gesetzt via Serial-Befehl
// `wifi_set <ssid> <psk>`. Nicht-blockierend: Connect läuft im Hintergrund,
// wir merken uns nur den State + IP für Status-Logs.
typedef struct {
  bool     enabled;
  bool     connected;
  char     ssid[33];
  char     psk[65];
  char     ip[16];      // dotted-quad als String
  uint32_t last_connect_attempt_ms;
} moki_wifi_t;
static moki_wifi_t g_wifi = { false, false, "", "", "", 0 };

static void wifi_load(void) {
  g_prefs.begin("moki", true);
  g_prefs.getString("wifi_ssid", g_wifi.ssid, sizeof(g_wifi.ssid));
  g_prefs.getString("wifi_psk",  g_wifi.psk,  sizeof(g_wifi.psk));
  g_wifi.enabled = (g_wifi.ssid[0] != 0);
  g_prefs.end();
  Serial.printf("[wifi] loaded creds: ssid='%s' enabled=%d\n",
                g_wifi.ssid, g_wifi.enabled);
}

static void wifi_save(void) {
  g_prefs.begin("moki", false);
  g_prefs.putString("wifi_ssid", g_wifi.ssid);
  g_prefs.putString("wifi_psk",  g_wifi.psk);
  g_prefs.end();
}

static void wifi_event_cb(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
      IPAddress ip = WiFi.localIP();
      snprintf(g_wifi.ip, sizeof(g_wifi.ip), "%u.%u.%u.%u",
               ip[0], ip[1], ip[2], ip[3]);
      g_wifi.connected = true;
      Serial.printf("[wifi] connected · ip=%s · rssi=%d\n",
                    g_wifi.ip, WiFi.RSSI());
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (g_wifi.connected) {
        Serial.println(F("[wifi] disconnected"));
      }
      g_wifi.connected = false;
      g_wifi.ip[0] = 0;
      break;
    default:
      break;
  }
}

static void wifi_init(void) {
  // WiFi lazy-init: claimt sich beim ersten WiFi.mode(WIFI_STA) ~20KB DRAM
  // dauerhaft. Wenn keine Creds: nicht init'en — sonst hat ein späterer
  // Scan-Versuch nicht genug Heap fürs net80211-Subsystem (ESP_ERR_NO_MEM).
  // Erste WiFi-Aktion (Scan oder Connect) macht die Init dann lazy.
  if (!g_wifi.enabled) {
    Serial.println(F("[wifi] no creds — STA off (lazy init on first use)"));
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(wifi_event_cb);
  Serial.printf("[wifi] connecting to '%s' ...\n", g_wifi.ssid);
  WiFi.begin(g_wifi.ssid, g_wifi.psk);
  g_wifi.last_connect_attempt_ms = millis();
}

static void wifi_loop(void) {
  if (!g_wifi.enabled) return;
  // Reconnect wenn disconnect älter als 30s
  if (!g_wifi.connected &&
      (millis() - g_wifi.last_connect_attempt_ms) > 30000) {
    Serial.printf("[wifi] reconnect '%s' ...\n", g_wifi.ssid);
    WiFi.disconnect();
    WiFi.begin(g_wifi.ssid, g_wifi.psk);
    g_wifi.last_connect_attempt_ms = millis();
  }
}

static void wifi_set_creds(const char *ssid, const char *psk) {
  strncpy(g_wifi.ssid, ssid, sizeof(g_wifi.ssid) - 1);
  g_wifi.ssid[sizeof(g_wifi.ssid) - 1] = 0;
  strncpy(g_wifi.psk, psk, sizeof(g_wifi.psk) - 1);
  g_wifi.psk[sizeof(g_wifi.psk) - 1] = 0;
  g_wifi.enabled = true;
  wifi_save();
  // Trigger fresh connect
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(wifi_event_cb);
  WiFi.disconnect();
  WiFi.begin(g_wifi.ssid, g_wifi.psk);
  g_wifi.last_connect_attempt_ms = millis();
  Serial.printf("[wifi] creds set, connecting to '%s'\n", g_wifi.ssid);
}

static void wifi_clear_creds(void) {
  g_wifi.ssid[0] = 0;
  g_wifi.psk[0] = 0;
  g_wifi.enabled = false;
  g_wifi.connected = false;
  wifi_save();
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  Serial.println(F("[wifi] cleared, STA off"));
}

// ── OTA-URL Persistenz (M8.7) ───────────────────────────────────────────
// User-konfigurierbare Default-URL für `ota_release`. Ideal: GitHub
// Releases /latest/download/firmware.bin — bleibt stabil, Mac muss nicht
// laufen. NVS-key: "ota_url".
// Default-URL (kompiliert eingebrannt). Kann via `ota_url <url>` überschrieben
// werden. Setzt damit auch das Henne-Ei-Problem von langen Befehlen via
// USB-CDC RX Buffer Limit (~64 Bytes) elegant außer Kraft.
static const char OTA_DEFAULT_URL[] =
  "https://github.com/Ovis-Shepherd/moki/releases/latest/download/firmware.bin";
static char g_ota_url[160] = "";

static void ota_url_load(void) {
  g_prefs.begin("moki", true);
  g_prefs.getString("ota_url", g_ota_url, sizeof(g_ota_url));
  g_prefs.end();
  if (!g_ota_url[0]) {
    strncpy(g_ota_url, OTA_DEFAULT_URL, sizeof(g_ota_url) - 1);
    g_ota_url[sizeof(g_ota_url) - 1] = 0;
    Serial.printf("[ota] using default URL: %s\n", g_ota_url);
  }
}
static void ota_url_save(const char *url) {
  strncpy(g_ota_url, url, sizeof(g_ota_url) - 1);
  g_ota_url[sizeof(g_ota_url) - 1] = 0;
  g_prefs.begin("moki", false);
  g_prefs.putString("ota_url", g_ota_url);
  g_prefs.end();
  Serial.printf("[ota] url saved: %s\n", g_ota_url);
}

// ── OTA-Pull (M8.3, M8.7 mit HTTPS) ──────────────────────────────────────
// Inline-OTA mit HTTPClient + Update.h. Akzeptiert http:// und https:// URLs.
// Bei https nutzen wir WiFiClientSecure im insecure-Modus (kein Cert-Check) —
// das spart Root-CA-Bundle und macht GitHub Releases / Cloudflare-Tunnel ohne
// Konfig erreichbar. Sicherheitsabwägung: Firmware-Blobs werden vom Bootloader
// per Hash validiert, Man-in-the-Middle könnte aber Update verhindern.
static bool ota_pull(const char *url) {
  if (!g_wifi.connected) {
    Serial.println(F("[ota] no WiFi — abort"));
    return false;
  }
  Serial.printf("[ota] pull %s\n", url);
  Serial.printf("[ota] free heap=%u psram=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  bool is_https = (strncmp(url, "https://", 8) == 0);
  WiFiClientSecure secure;
  WiFiClient plain;
  if (is_https) {
    secure.setInsecure();   // skip cert validation — simpler, no CA bundle
  }
  HTTPClient http;
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GH-Releases redirect
  bool began = is_https ? http.begin(secure, url) : http.begin(plain, url);
  if (!began) {
    Serial.println(F("[ota] http.begin failed"));
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota] HTTP %d\n", code);
    http.end();
    return false;
  }
  int total = http.getSize();
  if (total <= 0) {
    Serial.println(F("[ota] no Content-Length — abort"));
    http.end();
    return false;
  }
  Serial.printf("[ota] %d bytes, beginning Update.begin()\n", total);

  if (!Update.begin((size_t)total)) {
    Serial.printf("[ota] Update.begin failed: %s\n", Update.errorString());
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int written = 0;
  int last_pct = -1;
  while (http.connected() && written < total) {
    int avail = stream->available();
    if (avail <= 0) { delay(1); continue; }
    int to_read = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
    int got = stream->readBytes(buf, to_read);
    if (got <= 0) break;
    size_t w = Update.write(buf, got);
    if (w != (size_t)got) {
      Serial.printf("[ota] Update.write short: %u vs %d\n", (unsigned)w, got);
      Update.abort();
      http.end();
      return false;
    }
    written += got;
    int pct = (written * 100) / total;
    if (pct != last_pct && pct % 10 == 0) {
      Serial.printf("[ota] %d%% (%d/%d)\n", pct, written, total);
      last_pct = pct;
    }
  }
  http.end();

  if (written != total) {
    Serial.printf("[ota] short read: %d/%d\n", written, total);
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    Serial.printf("[ota] Update.end failed: %s\n", Update.errorString());
    return false;
  }
  Serial.println(F("[ota] OK — rebooting in 1s"));
  delay(1000);
  ESP.restart();
  return true;
}

#endif  // MOKI_WIFI

// ── Map data (M6.3 binary blob → PSRAM, M6.4 renderer) ────────────────────
// moki.map liegt auf LittleFS, wird beim Boot in PSRAM geladen. Format-
// Header siehe tools/map-pipeline/README.md.
//
// Layout im Speicher: header + raw line/point payload nacheinander. Wir
// behalten den ganzen Buffer und scannen ihn bei jedem Render. Bei 2.3MB
// und 200K Features ist das auf ESP32-S3 + PSRAM kein Problem.
typedef enum {
  MAP_T_RIVER    = 1,
  MAP_T_MOTORWAY = 2,
  MAP_T_PRIMARY  = 3,
  MAP_T_RAIL     = 4,
  MAP_T_BORDER   = 5,
} map_line_type_t;

typedef enum {
  MAP_P_CITY    = 1,
  MAP_P_TOWN    = 2,
  MAP_P_VILLAGE = 3,
  MAP_P_POI     = 4,
} map_point_type_t;

typedef enum {
  MAP_Z_COUNTRY = 0,
  MAP_Z_REGION  = 1,
  MAP_Z_CITY    = 2,
} map_zoom_t;

// Index pro Stadt (PSRAM), sortiert nach pop_log DESC. Erlaubt einen
// Render-Pass in Prioritäts-Reihenfolge → wir können kleine Städte
// drop'pen, wenn sie zu nah an einer schon gezeichneten Großstadt liegen.
typedef struct {
  uint32_t off;       // Byte-Offset in g_map.buf für diesen Stadteintrag
  int8_t   pop_log;   // Sort-Schlüssel (negativ wegen DESC-Sort)
} map_pt_idx_t;

typedef struct {
  uint8_t *buf;          // raw file bytes
  size_t   buf_len;
  bool     ready;
  // Decoded header
  float    lat_min, lat_max, lon_min, lon_max;
  uint32_t n_lines;
  uint32_t n_points;
  // Offsets into buf (after header)
  size_t   off_lines;
  size_t   off_points;
  // Sorted-by-pop city index (für Label-Collision)
  map_pt_idx_t *pt_idx;
  uint32_t      pt_idx_n;
} moki_map_t;
static moki_map_t g_map = { nullptr, 0, false, 0,0,0,0, 0,0, 0,0, nullptr, 0 };

static bool map_load(void) {
  if (!g_fs_ready) {
    Serial.println(F("[map] LittleFS not ready — skipping"));
    return false;
  }
  File f = LittleFS.open("/moki.map", "r");
  if (!f) {
    Serial.println(F("[map] /moki.map not found on LittleFS"));
    return false;
  }
  size_t sz = f.size();
  if (sz < 32) { f.close(); return false; }

  g_map.buf = (uint8_t *)ps_malloc(sz);
  if (!g_map.buf) {
    Serial.printf("[map] ps_malloc(%u) failed\n", (unsigned)sz);
    f.close();
    return false;
  }
  size_t got = f.read(g_map.buf, sz);
  f.close();
  if (got != sz) {
    Serial.printf("[map] read mismatch: got %u of %u\n",
                  (unsigned)got, (unsigned)sz);
    free(g_map.buf); g_map.buf = nullptr; return false;
  }
  g_map.buf_len = sz;

  // Parse header
  if (memcmp(g_map.buf, "MOKI", 4) != 0) {
    Serial.println(F("[map] bad magic"));
    free(g_map.buf); g_map.buf = nullptr; return false;
  }
  uint8_t version = g_map.buf[4];
  if (version != 1) {
    Serial.printf("[map] unsupported version %u\n", (unsigned)version);
    free(g_map.buf); g_map.buf = nullptr; return false;
  }
  memcpy(&g_map.lat_min, g_map.buf + 5,  4);
  memcpy(&g_map.lat_max, g_map.buf + 9,  4);
  memcpy(&g_map.lon_min, g_map.buf + 13, 4);
  memcpy(&g_map.lon_max, g_map.buf + 17, 4);
  memcpy(&g_map.n_lines,  g_map.buf + 21, 4);
  memcpy(&g_map.n_points, g_map.buf + 25, 4);
  // 3B reserved
  g_map.off_lines = 32;

  // Walk lines to find off_points
  size_t p = g_map.off_lines;
  for (uint32_t i = 0; i < g_map.n_lines; i++) {
    if (p + 4 > sz) {
      Serial.println(F("[map] truncated lines"));
      free(g_map.buf); g_map.buf = nullptr; return false;
    }
    uint16_t n_pts;
    memcpy(&n_pts, g_map.buf + p + 2, 2);
    p += 4 + (size_t)n_pts * 4;
  }
  g_map.off_points = p;

  // Build sorted point index (pop_log DESC, also größere Städte zuerst).
  // qsort verlangt int_compare; wir negieren pop_log um DESC zu kriegen.
  if (g_map.n_points > 0) {
    g_map.pt_idx = (map_pt_idx_t *)ps_malloc(sizeof(map_pt_idx_t) * g_map.n_points);
    if (g_map.pt_idx) {
      size_t poff = g_map.off_points;
      uint32_t k = 0;
      for (uint32_t i = 0; i < g_map.n_points && poff + 8 <= g_map.buf_len; i++) {
        uint8_t  pop_log  = g_map.buf[poff + 6];
        uint8_t  name_len = g_map.buf[poff + 7];
        g_map.pt_idx[k].off     = (uint32_t)poff;
        g_map.pt_idx[k].pop_log = (int8_t)pop_log;
        k++;
        poff += 8 + name_len;
      }
      g_map.pt_idx_n = k;
      // Inline insertion-sort — n_points typ. < 1000, k*n bounded
      for (uint32_t i = 1; i < g_map.pt_idx_n; i++) {
        map_pt_idx_t cur = g_map.pt_idx[i];
        int j = (int)i - 1;
        while (j >= 0 && g_map.pt_idx[j].pop_log < cur.pop_log) {
          g_map.pt_idx[j + 1] = g_map.pt_idx[j];
          j--;
        }
        g_map.pt_idx[j + 1] = cur;
      }
    } else {
      Serial.println(F("[map] pt_idx ps_malloc failed — labels will be unsorted"));
    }
  }

  g_map.ready = true;
  Serial.printf("[map] ready: %u lines, %u points (idx=%u), %.2f MB, bbox=[%.3f,%.3f,%.3f,%.3f]\n",
                (unsigned)g_map.n_lines, (unsigned)g_map.n_points,
                (unsigned)g_map.pt_idx_n,
                sz / 1024.0f / 1024.0f,
                g_map.lat_min, g_map.lat_max, g_map.lon_min, g_map.lon_max);
  return true;
}

// Convert a quantized 16-bit lat/lon back to degrees
static inline float map_q_to_lat(uint16_t q) {
  return g_map.lat_min + (g_map.lat_max - g_map.lat_min) * q / 65535.0f;
}
static inline float map_q_to_lon(uint16_t q) {
  return g_map.lon_min + (g_map.lon_max - g_map.lon_min) * q / 65535.0f;
}

// Viewport: was sehen wir gerade?
typedef struct {
  float lat_center;
  float lon_center;
  uint8_t zoom_idx;          // 0=country, 1=region, 2=city
  // Computed jeweils beim Render-Aufruf:
  float px_per_deg_lat;
  float px_per_deg_lon;
} moki_viewport_t;

// Default: Heidelberg-Altstadt, city zoom (~11 km × 20 km Sicht — gut zum
// Radfahren/Spazieren, halb Heidelberg + ein bisschen Umland)
static moki_viewport_t g_viewport = {
  /*lat_center*/ 49.4099f,
  /*lon_center*/ 8.6943f,
  /*zoom_idx*/   MAP_Z_CITY,
  /*px_per_deg_*/ 0.0f, 0.0f
};

// Pixels per degree, je nach zoom_idx. Display ist 540×960 Hochformat.
static void viewport_compute_scale(moki_viewport_t *v, int screen_w, int screen_h) {
  // Anvisierte vertikale Sicht (Höhe in Grad) je Zoom-Stufe:
  static const float view_h_deg[] = {
    5.0f,      // country: ~550 km vertikal
    1.0f,      // region:  ~110 km
    0.10f      // city:    ~11 km
  };
  uint8_t z = v->zoom_idx > 2 ? 2 : v->zoom_idx;
  float h_deg = view_h_deg[z];
  v->px_per_deg_lat = (float)screen_h / h_deg;
  // Längengrade werden mit cos(Breite) zusammengezogen → korrekt skaliert,
  // sonst sieht alles in Norddeutschland gestaucht aus
  float coslat = cosf(v->lat_center * (float)M_PI / 180.0f);
  v->px_per_deg_lon = v->px_per_deg_lat * coslat;
}

// Project (lat, lon) → screen px relative to viewport center at (cx, cy)
static inline void map_project(const moki_viewport_t *v,
                                float lat, float lon,
                                int cx, int cy,
                                int *out_x, int *out_y) {
  float dx = (lon - v->lon_center) * v->px_per_deg_lon;
  float dy = -(lat - v->lat_center) * v->px_per_deg_lat;  // y inverts
  *out_x = cx + (int)dx;
  *out_y = cy + (int)dy;
}

// ── SD-Card book index (M5-Full step 1) ──────────────────────────────────
// Plain .txt files on microSD, one per book. EPUB-zip parsing comes later.
// Books live in /books/ at SD root. We index them lazily on tab-open.
#define MOKI_MAX_BOOKS 12
typedef struct {
  char path[40];   // "/books/walden.txt"
  char title[32];  // pretty filename ("walden")
} moki_book_t;
static moki_book_t *g_books = nullptr;     // [MOKI_MAX_BOOKS] PSRAM-heap
static int g_book_count = 0;

static void sd_mount(void) {
  // SD shares SPI with the LoRa radio. SPI.begin() was called by target.cpp's
  // radio_init() so we just need to assert the right CS pin and let SD.begin
  // probe.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);   // de-assert until SD.begin
  if (SD.begin(SD_CS, SPI, 4000000)) {
    g_sd_ready = true;
    Serial.printf("[sd] mounted: type=%d size=%lluMB\n",
                  SD.cardType(), SD.cardSize() / (1024ULL * 1024ULL));
  } else {
    Serial.println(F("[sd] mount failed (no card? wrong CS? format?)"));
  }
}

// New book entry: source flag tells us where to read from.
// Re-using moki_book_t — we encode source in the path prefix:
//   "/books/foo.txt"        → SD
//   "lfs:/books/foo.txt"    → LittleFS
// book_read_chunk + book_total_length below sniff the prefix.

// Scan /books/ on a FS. Returns count of books added.
template <typename FS>
static int scan_books_on_fs(FS &fs, const char *path_prefix, bool is_lfs) {
  int added = 0;
  File dir = fs.open(is_lfs ? "/books" : "/books");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  while (g_book_count < MOKI_MAX_BOOKS) {
    File f = dir.openNextFile();
    if (!f) break;
    if (f.isDirectory()) { f.close(); continue; }
    const char *name = f.name();
    size_t nlen = strlen(name);
    if (nlen < 5 || strcmp(name + nlen - 4, ".txt") != 0) { f.close(); continue; }

    moki_book_t *b = &g_books[g_book_count];
    snprintf(b->path, sizeof(b->path), "%s/%s", path_prefix, name);
    size_t copy_len = nlen - 4;
    if (copy_len > sizeof(b->title) - 1) copy_len = sizeof(b->title) - 1;
    memcpy(b->title, name, copy_len);
    b->title[copy_len] = 0;
    g_book_count++;
    added++;
    f.close();
  }
  dir.close();
  return added;
}

static int books_scan(void) {
  g_book_count = 0;
  int from_lfs = 0, from_sd = 0;
  // LittleFS first — these are firmware-bundled books (e.g., Nietzsche
  // uploaded via `pio run -t uploadfs`). Always available, no SD needed.
  if (g_fs_ready) from_lfs = scan_books_on_fs(LittleFS, "lfs:/books", true);
  if (g_sd_ready) from_sd  = scan_books_on_fs(SD, "/books", false);
  Serial.printf("[books] indexed %d books (lfs=%d sd=%d)\n",
                g_book_count, from_lfs, from_sd);
  return g_book_count;
}

static void fs_mount(void) {
  // .begin(format_on_fail=true) → if first boot or partition broken, format.
  if (LittleFS.begin(true)) {
    g_fs_ready = true;
    Serial.printf("[fs] LittleFS mounted: total=%u used=%u\n",
                  (unsigned)LittleFS.totalBytes(),
                  (unsigned)LittleFS.usedBytes());
  } else {
    Serial.println(F("[fs] LittleFS mount FAILED — disk persistence disabled"));
  }
}

// Forward decl — defined in the LittleFS section below.
static void lora_persist_append(const moki_lora_msg_t *m);

// Variant that also captures SNR and raw bytes (no NUL-termination assumption).
static void lora_push_msg_raw(const char *from, const uint8_t *raw, size_t len,
                              int16_t rssi, float snr) {
  moki_lora_msg_t *m = &g_lora_msgs[g_lora_msg_head];
  strncpy(m->from, from, sizeof(m->from)-1); m->from[sizeof(m->from)-1] = 0;
  if (len > sizeof(m->text)) len = sizeof(m->text);
  memcpy(m->text, raw, len);
  m->len     = (uint8_t)len;
  m->rssi    = rssi;
  m->snr_x10 = (int8_t)(snr * 10.0f);
  m->preset  = g_settings.lora_preset;
  m->channel_idx = -2;   // raw RX = foreign-protocol bucket
  m->ts_ms   = millis();
  g_lora_msg_head = (g_lora_msg_head + 1) % LORA_MSG_CAP;
  if (g_lora_msg_count < LORA_MSG_CAP) g_lora_msg_count++;
  lora_persist_append(m);
}

static void lora_persist_append(const moki_lora_msg_t *m) {
  if (!g_fs_ready) return;
  // Soft size cap: drop the file if it grows too big — simpler than full
  // log compaction. Lucas can `lora_clear` anytime to reset deliberately.
  if (LittleFS.exists(LORA_LOG_PATH)) {
    File probe = LittleFS.open(LORA_LOG_PATH, "r");
    if (probe && probe.size() >= LORA_LOG_MAX_BYTES) {
      probe.close();
      LittleFS.remove(LORA_LOG_PATH);
      Serial.println(F("[fs] lora.log capped — truncated to 0"));
    } else if (probe) {
      probe.close();
    }
  }
  File f = LittleFS.open(LORA_LOG_PATH, "a");
  if (!f) return;
  f.write((const uint8_t *)m, sizeof(*m));
  f.close();
}

static void lora_persist_load(void) {
  if (!g_fs_ready) return;
  if (!LittleFS.exists(LORA_LOG_PATH)) return;
  File f = LittleFS.open(LORA_LOG_PATH, "r");
  if (!f) return;
  size_t total = f.size();
  size_t rec   = sizeof(moki_lora_msg_t);
  if (total < rec) { f.close(); return; }

  // Schema sanity check — if file size isn't a clean multiple of the
  // current record size, the struct layout changed since these records
  // were written (e.g. a new field appended). Drop the log to avoid
  // misalignment and start fresh.
  if ((total % rec) != 0) {
    f.close();
    LittleFS.remove(LORA_LOG_PATH);
    Serial.printf("[fs] lora.log size %u not multiple of record %u — discarded (schema changed)\n",
                  (unsigned)total, (unsigned)rec);
    return;
  }

  size_t n_rec = total / rec;
  // Read only the most-recent LORA_MSG_CAP records into the ring buffer.
  size_t skip = (n_rec > LORA_MSG_CAP) ? (n_rec - LORA_MSG_CAP) : 0;
  f.seek(skip * rec);
  size_t loaded = 0;
  while (loaded < LORA_MSG_CAP) {
    moki_lora_msg_t m;
    size_t got = f.read((uint8_t *)&m, rec);
    if (got != rec) break;
    // Defensive clamp — if channel_idx is out of valid range (e.g. migrated
    // legacy record where the field was uninitialised), treat as direct-msg
    // bucket so it shows up regardless of which channel is selected.
    if (m.channel_idx < -2 || m.channel_idx > 3) m.channel_idx = -1;
    g_lora_msgs[g_lora_msg_head] = m;
    g_lora_msg_head = (g_lora_msg_head + 1) % LORA_MSG_CAP;
    if (g_lora_msg_count < LORA_MSG_CAP) g_lora_msg_count++;
    loaded++;
  }
  f.close();
  Serial.printf("[fs] loaded %u lora msgs from disk (file had %u total)\n",
                (unsigned)loaded, (unsigned)n_rec);
}

static void lora_persist_clear(void) {
  if (!g_fs_ready) return;
  if (LittleFS.exists(LORA_LOG_PATH)) LittleFS.remove(LORA_LOG_PATH);
  Serial.println(F("[fs] lora.log removed"));
}

static void state_save_settings(void) {
  g_prefs.begin("moki", false);
  g_prefs.putUInt("settings_v", SETTINGS_SCHEMA_V);
  g_prefs.putBytes("settings", &g_settings, sizeof(g_settings));
  g_prefs.end();
  Serial.println(F("[persist] saved settings"));
}
// ── Multi-Channel persist (NVS blob "channels") ──────────────────────────
static void state_save_channels(void) {
  g_prefs.begin("moki", false);
  g_prefs.putUInt("channels_v", CHANNELS_SCHEMA_V);
  g_prefs.putUInt("channels_n", (uint32_t)g_num_channels);
  if (g_num_channels > 0) {
    g_prefs.putBytes("channels", g_channels,
                     sizeof(moki_channel_t) * g_num_channels);
  }
  g_prefs.end();
  Serial.printf("[persist] saved %d channels\n", g_num_channels);
}
static void state_load_channels(void) {
  g_prefs.begin("moki", true);
  uint32_t v = g_prefs.getUInt("channels_v", 0);
  uint32_t n = g_prefs.getUInt("channels_n", 0xFFFFFFFFu);
  bool ok = (n != 0xFFFFFFFFu && n <= MAX_MOKI_CHANNELS);
  if (ok && v == CHANNELS_SCHEMA_V) {
    g_num_channels = (int)n;
    if (g_num_channels > 0) {
      g_prefs.getBytes("channels", g_channels,
                       sizeof(moki_channel_t) * g_num_channels);
    }
    g_prefs.end();
    Serial.printf("[persist] loaded %d channels\n", g_num_channels);
  } else {
    g_prefs.end();
    // Bootstrap from legacy single-channel settings: copy mesh_channel_*
    // into g_channels[0] so existing users seamlessly migrate.
    g_num_channels = 1;
    strncpy(g_channels[0].name, g_settings.mesh_channel_name,
            sizeof(g_channels[0].name) - 1);
    g_channels[0].name[sizeof(g_channels[0].name) - 1] = 0;
    strncpy(g_channels[0].psk_b64, g_settings.mesh_channel_psk_b64,
            sizeof(g_channels[0].psk_b64) - 1);
    g_channels[0].psk_b64[sizeof(g_channels[0].psk_b64) - 1] = 0;
    g_channels[0].active = true;
    state_save_channels();
    Serial.println(F("[persist] channels bootstrapped from legacy settings"));
  }
}

// Add a new channel (returns false if already at max).
static bool channels_add(const char *name, const char *psk_b64) {
  if (g_num_channels >= MAX_MOKI_CHANNELS) return false;
  strncpy(g_channels[g_num_channels].name, name,
          sizeof(g_channels[g_num_channels].name) - 1);
  g_channels[g_num_channels].name[sizeof(g_channels[g_num_channels].name) - 1] = 0;
  strncpy(g_channels[g_num_channels].psk_b64, psk_b64,
          sizeof(g_channels[g_num_channels].psk_b64) - 1);
  g_channels[g_num_channels].psk_b64[sizeof(g_channels[g_num_channels].psk_b64) - 1] = 0;
  g_channels[g_num_channels].active = true;
  g_num_channels++;
  state_save_channels();
  return true;
}

// Remove channel by index (returns false if out of range or last channel).
static bool channels_remove(int idx) {
  if (idx < 0 || idx >= g_num_channels) return false;
  if (g_num_channels <= 1) return false;  // keep at least one
  for (int i = idx; i < g_num_channels - 1; i++) {
    g_channels[i] = g_channels[i + 1];
  }
  g_num_channels--;
  if (g_settings.mesh_active_channel >= g_num_channels) {
    g_settings.mesh_active_channel = 0;
    state_save_settings();
  }
  state_save_channels();
  return true;
}

// ── M5-Lite: Reader pagination state ──────────────────────────────────────
// Saved in NVS as a uint32 under "book_p" so Lucas's last-read page persists
// across reboots. One book for now; multi-book index lands in M5-Full.
static void state_save_book_page(void) {
  g_prefs.begin("moki", false);
  g_prefs.putUInt(BOOK_PAGE_KEY, (uint32_t)g_book_page);
  g_prefs.end();
}
static void state_load_book_page(void) {
  g_prefs.begin("moki", true);
  g_book_page = (int)g_prefs.getUInt(BOOK_PAGE_KEY, 0);
  g_prefs.end();
}

// Returns true if the active book's path is on LittleFS, else SD.
static bool active_book_is_lfs(void) {
  if (g_book_active_idx < 0 || g_book_active_idx >= g_book_count) return false;
  return strncmp(g_books[g_book_active_idx].path, "lfs:", 4) == 0;
}

// Open helper that picks the right FS based on path prefix.
static File book_open_active(void) {
  if (g_book_active_idx < 0 || g_book_active_idx >= g_book_count) return File();
  const char *p = g_books[g_book_active_idx].path;
  if (strncmp(p, "lfs:", 4) == 0) return LittleFS.open(p + 4);
  return SD.open(p);
}

// ── M5-Full step 2: file-backed pagination ────────────────────────────────
static size_t book_total_length(void) {
  if (g_book_active_idx == -2) return strlen_P(MOKI_MANUAL_TEXT);
  if (g_book_active_idx == -1) return strlen_P(MOKI_BOOK_TEXT);
  File f = book_open_active();
  if (!f) return 0;
  size_t sz = f.size();
  f.close();
  return sz;
}

// Replace UTF-8 codepoints that aren't in our Fraunces/JetBrains font glyph
// sets with ASCII fallbacks, in-place. The fonts cover:
//   ASCII + ä/ö/ü/ß + Ä/Ö/Ü + middle-dot (·) + en-dash (–) + arrow (→ mono)
// Everything else (smart quotes „ " ' ', em-dash —, ellipsis …, guillemets
// « », NBSP) renders as a placeholder rectangle on the e-ink display. We
// transliterate to plain ASCII so books read cleanly without regenerating
// the fonts. Returns the new (possibly shorter) length.
static size_t normalize_utf8_for_font(char *buf, size_t len) {
  size_t r = 0, w = 0;
  while (r < len) {
    unsigned char c = (unsigned char)buf[r];
    if (c < 0x80) { buf[w++] = buf[r++]; continue; }

    if ((c & 0xE0) == 0xC0) {
      if (r + 1 >= len) break;   // truncated — drop trailing
      unsigned char c2 = (unsigned char)buf[r + 1];
      // Keep all 0xC3 (ä/ö/ü/ß/Ä/Ö/Ü, etc.) and middle-dot.
      if (c == 0xC3 || (c == 0xC2 && c2 == 0xB7)) {
        buf[w++] = c; buf[w++] = c2; r += 2; continue;
      }
      if (c == 0xC2) {
        if      (c2 == 0xAB || c2 == 0xBB) buf[w++] = '"';   // « »
        else if (c2 == 0xA0)               buf[w++] = ' ';   // NBSP
        // 0xAD soft-hyphen + others: drop
        r += 2; continue;
      }
      r += 2; continue;
    }

    if ((c & 0xF0) == 0xE0) {
      if (r + 2 >= len) break;   // truncated
      unsigned char c2 = (unsigned char)buf[r + 1];
      unsigned char c3 = (unsigned char)buf[r + 2];
      // en-dash (–) and arrow (→) are in our fonts — keep
      if (c == 0xE2 && c2 == 0x80 && c3 == 0x93) {
        buf[w++] = c; buf[w++] = c2; buf[w++] = c3; r += 3; continue;
      }
      if (c == 0xE2 && c2 == 0x86 && c3 == 0x92) {
        buf[w++] = c; buf[w++] = c2; buf[w++] = c3; r += 3; continue;
      }
      // General Punctuation block: 0xE2 0x80 ..
      if (c == 0xE2 && c2 == 0x80) {
        switch (c3) {
          case 0x90: case 0x91: case 0x92:        // hyphen variants
            buf[w++] = '-'; break;
          case 0x94: case 0x95:                   // em-dash, horizontal bar → en-dash
            buf[w++] = 0xE2; buf[w++] = 0x80; buf[w++] = 0x93; break;
          case 0x98: case 0x99: case 0x9B: case 0x82:  // single quotes
            buf[w++] = '\''; break;
          case 0x9A:                              // ‚ → ,
            buf[w++] = ','; break;
          case 0x9C: case 0x9D: case 0x9E: case 0x9F:  // double quotes
            buf[w++] = '"'; break;
          case 0xA0: case 0xA1: case 0xA2: case 0xA3:  // various spaces
            buf[w++] = ' '; break;
          case 0xA6:                              // ellipsis …
            buf[w++] = '.'; buf[w++] = '.'; buf[w++] = '.'; break;
          default: break;                         // drop unknown
        }
        r += 3; continue;
      }
      r += 3; continue;
    }

    if ((c & 0xF8) == 0xF0) {
      if (r + 3 >= len) break;
      r += 4; continue;
    }
    r++;
  }
  buf[w] = 0;
  return w;
}

static size_t book_read_chunk(size_t start, size_t want, char *out_buf, size_t buf_size) {
  if (want > buf_size - 1) want = buf_size - 1;
  size_t got = 0;
  if (g_book_active_idx == -2 || g_book_active_idx == -1) {
    const char *src = (g_book_active_idx == -2) ? MOKI_MANUAL_TEXT : MOKI_BOOK_TEXT;
    size_t total = strlen_P(src);
    if (start + want > total) want = (start < total) ? total - start : 0;
    memcpy_P(out_buf, src + start, want);
    out_buf[want] = 0;
    got = want;
  } else {
    File f = book_open_active();
    if (!f) { out_buf[0] = 0; return 0; }
    if (!f.seek(start)) { f.close(); out_buf[0] = 0; return 0; }
    got = f.read((uint8_t *)out_buf, want);
    out_buf[got] = 0;
    f.close();
  }
  // Replace smart quotes / ellipsis / em-dash etc. with ASCII fallbacks
  // so they render properly with the bundled font glyphs.
  return normalize_utf8_for_font(out_buf, got);
}

// Tap on a book in the list — switch active book + reset page + bookmark.
// Bookmark scheme: NVS key "book_p_<idx>" for SD books; "book_p" for Walden;
// "book_p_m" for the manual.
static const char *bookmark_key_for(int idx, char *buf, size_t buf_size) {
  if (idx == -2) return "book_p_m";
  if (idx == -1) return "book_p";
  snprintf(buf, buf_size, "book_p_%d", idx);
  return buf;
}

static void book_select(int idx) {
  if (idx < -2 || idx >= g_book_count) return;
  // Save current page under its current key
  char buf[16];
  const char *cur_key = bookmark_key_for(g_book_active_idx, buf, sizeof(buf));
  g_prefs.begin("moki", false);
  g_prefs.putUInt(cur_key, (uint32_t)g_book_page);
  g_prefs.end();
  // Switch + load new page
  g_book_active_idx = idx;
  const char *new_key = bookmark_key_for(idx, buf, sizeof(buf));
  g_prefs.begin("moki", true);
  g_book_page = (int)g_prefs.getUInt(new_key, 0);
  g_prefs.end();
}

// ── Mesh-Identity (32-byte secret) ───────────────────────────────────────
// Holds a per-device 32-byte secret. Used as:
//   - PSK material for Moki↔Moki encrypted chat (AES-128 derived from first 16B)
//   - Future MeshCore Ed25519 seed (when we wire BaseChatMesh in Phase 2c)
// Stored under NVS key "identity" (32 raw bytes).
//
// Generated lazily: first boot creates it from RadioLib RNG (best entropy
// source we have until MeshCore is active). Persisted thereafter.
#define IDENTITY_SECRET_SIZE 32
// Non-static: moki_mesh.cpp references this for Ed25519 key derivation.
uint8_t g_identity_secret[IDENTITY_SECRET_SIZE] = {0};
static bool    g_identity_ready = false;

static void state_load_identity(void) {
  g_prefs.begin("moki", true);
  size_t got = g_prefs.getBytes("identity", g_identity_secret, IDENTITY_SECRET_SIZE);
  g_prefs.end();
  if (got == IDENTITY_SECRET_SIZE) {
    g_identity_ready = true;
    Serial.printf("[persist] identity loaded (first byte: 0x%02x)\n",
                  g_identity_secret[0]);
  } else {
    Serial.println(F("[persist] identity not yet generated"));
  }
}

// Call after lora_init so we have RadioLib RNG available.
static void state_ensure_identity(void) {
  if (g_identity_ready) return;
  // Use SX1262 random hardware (RSSI noise) — best entropy source on this MCU.
  for (int i = 0; i < IDENTITY_SECRET_SIZE; i++) {
    if (g_lora_ready) {
      g_identity_secret[i] = (uint8_t)(g_radio.random(256));
    } else {
      // Fallback: ESP32 hardware RNG via esp_random()
      g_identity_secret[i] = (uint8_t)(esp_random() & 0xFF);
    }
  }
  g_prefs.begin("moki", false);
  g_prefs.putBytes("identity", g_identity_secret, IDENTITY_SECRET_SIZE);
  g_prefs.end();
  g_identity_ready = true;
  Serial.printf("[persist] generated new identity (first byte: 0x%02x)\n",
                g_identity_secret[0]);
}

static void state_load_settings(void) {
  // Backward-compat: older NVS blobs are smaller (no lora_preset). Read up to
  // the actual stored size; uninitialized tail keeps the defaults from the
  // global initializer.
  g_prefs.begin("moki", true);
  size_t got = g_prefs.getBytes("settings", &g_settings, sizeof(g_settings));
  g_prefs.end();
  if (got >= sizeof(g_settings) - 1) {
    Serial.printf("[persist] loaded settings (sync=%u handle='%s' preset=%u)\n",
                  (unsigned)g_settings.sync_interval_min, g_settings.handle,
                  (unsigned)g_settings.lora_preset);
  } else if (got > 0) {
    Serial.printf("[persist] migrated old settings (%u→%u bytes), preset default\n",
                  (unsigned)got, (unsigned)sizeof(g_settings));
    g_settings.lora_preset = LORA_PRESET_MESHCORE_NARROW;
    state_save_settings();
  } else {
    Serial.println(F("[persist] settings empty — using defaults"));
    state_save_settings();
  }
}
typedef enum { DO_HABITS = 0, DO_TODOS, DO_CALENDAR } do_tab_t;
typedef enum { READ_BOOK = 0, READ_FEED, READ_NOTES } read_tab_t;
typedef enum { MAP_MAP   = 0, MAP_NEARBY }            map_tab_t;

static screen_id_t current_screen  = SCR_HOME;
static do_tab_t    current_do_tab  = DO_TODOS;
static read_tab_t  current_read_tab = READ_BOOK;
static map_tab_t   current_map_tab = MAP_MAP;

static void switch_screen(screen_id_t to);
static void build_home(void);
static void build_do(void);
static void build_read(void);
static void build_chats(void);
static void build_map(void);
static void build_mood(void);
static void build_profile(void);
static void build_note_new(void);
static void build_note_edit(void);
static void on_mood_pill_clicked(lv_event_t *e);
static void on_profile_clicked(lv_event_t *e);
static void on_back_home(lv_event_t *e);
static void show_toast(const char *text);
static void on_note_open(lv_event_t *e);
static void on_note_new(lv_event_t *e);
static void on_template_picked(lv_event_t *e);
static void on_note_back_to_read(lv_event_t *e);
static void on_note_pin_toggle(lv_event_t *e);
static void on_note_delete(lv_event_t *e);
static void on_note_mode_toggle(lv_event_t *e);
void open_compose_todo(void);
void open_compose_habit(void);
void open_compose_event(void);

// Active note + edit-mode state
static int g_active_note   = -1;          // index into g_notes
static bool g_note_write   = false;       // false = read mode, true = write mode
static char g_note_title_buf[64];
static char g_note_body_buf[1024];
static lv_obj_t *g_note_title_label = NULL;
static lv_obj_t *g_note_body_label  = NULL;

static void state_save_notes(void) {
  g_prefs.begin("moki", false);
  g_prefs.putUInt("notes_v", NOTES_SCHEMA_V);
  g_prefs.putUInt("notes_n", (uint32_t)g_notes_count);
  if (g_notes_count > 0)
    g_prefs.putBytes("notes", g_notes, sizeof(moki_note_t) * g_notes_count);
  else
    g_prefs.remove("notes");
  g_prefs.end();
  Serial.printf("[persist] saved %d notes\n", g_notes_count);
}
static void state_load_notes(void) {
  g_prefs.begin("moki", true);
  uint32_t v = g_prefs.getUInt("notes_v", 0);
  uint32_t n = g_prefs.getUInt("notes_n", 0xFFFFFFFFu);
  if (v == NOTES_SCHEMA_V && n != 0xFFFFFFFFu && n <= MAX_NOTES) {
    g_notes_count = (int)n;
    if (g_notes_count > 0)
      g_prefs.getBytes("notes", g_notes, sizeof(moki_note_t) * g_notes_count);
    g_prefs.end();
    Serial.printf("[persist] loaded %d notes (v%u)\n", g_notes_count, v);
  } else {
    g_prefs.end();
    Serial.println(F("[persist] notes empty — bootstrapping"));
    state_init_notes();
    state_save_notes();
  }
}

static void state_save_mood(void) {
  g_prefs.begin("moki", false);
  g_prefs.putString("mood", g_active_mood);
  g_prefs.end();
  Serial.printf("[persist] saved mood='%s'\n", g_active_mood);
}
static void state_load_mood(void) {
  g_prefs.begin("moki", true);
  String s = g_prefs.getString("mood", "");
  g_prefs.end();
  strncpy(g_active_mood, s.c_str(), sizeof(g_active_mood)-1);
  g_active_mood[sizeof(g_active_mood)-1] = 0;
  Serial.printf("[persist] loaded mood='%s'\n", g_active_mood);
}

// Forward decl for the dock-handler shortcut to lora chat.
extern int g_active_chat;

static void on_dock_clicked(lv_event_t *e) {
  intptr_t idx = (intptr_t)lv_event_get_user_data(e);
  static const screen_id_t mapping[] = { SCR_HOME, SCR_DO, SCR_READ, SCR_CHAT, SCR_MAP };
  if (idx >= 0 && idx < 5) {
    Serial.printf("[nav] dock → screen %d\n", (int)mapping[idx]);
    switch_screen(mapping[idx]);
  }
}

static void on_do_tab_clicked(lv_event_t *e) {
  current_do_tab = (do_tab_t)(intptr_t)lv_event_get_user_data(e);
  switch_screen(SCR_DO);
}
static void on_read_tab_clicked(lv_event_t *e) {
  current_read_tab = (read_tab_t)(intptr_t)lv_event_get_user_data(e);
  switch_screen(SCR_READ);
}
static void on_map_tab_clicked(lv_event_t *e) {
  current_map_tab = (map_tab_t)(intptr_t)lv_event_get_user_data(e);
  switch_screen(SCR_MAP);
}

// ============================================================================
// Compose sheet — shared state + helpers
// ============================================================================
typedef enum { COMPOSE_TODO, COMPOSE_HABIT, COMPOSE_EVENT } compose_kind_t;
static compose_kind_t g_compose_kind     = COMPOSE_TODO;
static char        g_compose_title[64]   = "";
static char        g_compose_cat[16]     = "self";
static char        g_compose_deadline[24]= "";
static bool        g_compose_recurring   = false;
static lv_obj_t   *g_compose_overlay     = NULL;
static lv_obj_t   *g_compose_title_label = NULL;
static void        build_compose_overlay(void);

static void compose_close(void) {
  if (g_compose_overlay) {
    lv_obj_del(g_compose_overlay);
    g_compose_overlay = NULL;
    g_compose_title_label = NULL;
  }
}

static void compose_save(void) {
  if (g_compose_title[0] == 0) return;

  if (g_compose_kind == COMPOSE_TODO) {
    if (g_todos_count >= MAX_TODOS) return;
    moki_todo_t *t = &g_todos[g_todos_count++];
    strncpy(t->title,    g_compose_title,    sizeof(t->title)-1);    t->title[sizeof(t->title)-1] = 0;
    strncpy(t->cat,      g_compose_cat,      sizeof(t->cat)-1);      t->cat[sizeof(t->cat)-1] = 0;
    strncpy(t->deadline, g_compose_deadline, sizeof(t->deadline)-1); t->deadline[sizeof(t->deadline)-1] = 0;
    t->recurring = g_compose_recurring;
    t->done      = false;
    Serial.printf("[compose] saved todo: '%s'\n", t->title);
    state_save_todos();
    show_toast("AUFGABE GESPEICHERT");
  } else if (g_compose_kind == COMPOSE_HABIT) {
    if (g_habits_count >= MAX_HABITS) return;
    moki_habit_t *h = &g_habits[g_habits_count++];
    strncpy(h->name, g_compose_title, sizeof(h->name)-1); h->name[sizeof(h->name)-1] = 0;
    h->today_count = 0;
    h->streak      = 0;
    Serial.printf("[compose] saved habit: '%s'\n", h->name);
    state_save_habits();
    show_toast("GEWOHNHEIT GESPEICHERT");
  } else { // COMPOSE_EVENT
    if (g_events_count >= MAX_EVENTS) return;
    moki_event_t *ev = &g_events[g_events_count++];
    strncpy(ev->title, g_compose_title, sizeof(ev->title)-1); ev->title[sizeof(ev->title)-1] = 0;
    strncpy(ev->place, g_compose_cat,   sizeof(ev->place)-1); ev->place[sizeof(ev->place)-1] = 0;
    strcpy(ev->kind, "private");
    strcpy(ev->hour, "19:00");
    ev->day = 2;
    Serial.printf("[compose] saved event: '%s'\n", ev->title);
    state_save_events();
    show_toast("TERMIN GESPEICHERT");
  }

  g_compose_title[0]    = 0;
  strcpy(g_compose_cat, "self");
  g_compose_deadline[0] = 0;
  g_compose_recurring   = false;
  compose_close();
  // Re-arm the DO screen so we pick the right tab
  current_do_tab = (g_compose_kind == COMPOSE_TODO) ? DO_TODOS
                 : (g_compose_kind == COMPOSE_HABIT) ? DO_HABITS
                                                    : DO_CALENDAR;
  switch_screen(SCR_DO);
}

static void key_event(const char *key) {
  size_t len = strlen(g_compose_title);
  if (!strcmp(key,"BACK")) {
    if (len > 0) {
      // walk back over a UTF-8 continuation byte if necessary
      do { len--; } while (len > 0 && (((unsigned char)g_compose_title[len]) & 0xC0) == 0x80);
      g_compose_title[len] = 0;
    }
  } else if (!strcmp(key,"SPACE")) {
    if (len + 1 < sizeof(g_compose_title)) {
      g_compose_title[len] = ' '; g_compose_title[len+1] = 0;
    }
  } else {
    size_t klen = strlen(key);
    if (len + klen + 1 < sizeof(g_compose_title)) {
      memcpy(g_compose_title + len, key, klen);
      g_compose_title[len + klen] = 0;
    }
  }
  if (g_compose_title_label) {
    lv_label_set_text(g_compose_title_label, g_compose_title[0] ? g_compose_title : "…");
  }
}

static void on_key_clicked(lv_event_t *e) {
  key_event((const char *)lv_event_get_user_data(e));
}

static void build_keyboard_row(lv_obj_t *parent, const char *const *keys, int n) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 56);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);

  for (int i = 0; i < n; i++) {
    lv_obj_t *btn = lv_obj_create(row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 48, 50);
    lv_obj_set_style_bg_color(btn, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 2, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_key_clicked, LV_EVENT_CLICKED, (void *)keys[i]);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, keys[i]);
    lv_obj_set_style_text_font(l, &moki_fraunces_regular_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_center(l);
  }
}

static void on_cancel_clicked(lv_event_t *e) {
  g_compose_title[0] = 0;
  compose_close();
}
static void on_save_clicked(lv_event_t *e) {
  compose_save();
}
static void on_cat_chip_clicked(lv_event_t *e) {
  const char *id = (const char *)lv_event_get_user_data(e);
  strncpy(g_compose_cat, id, sizeof(g_compose_cat)-1);
  g_compose_cat[sizeof(g_compose_cat)-1] = 0;
  build_compose_overlay();   // re-render to flip chip styles
}
static void on_dl_chip_clicked(lv_event_t *e) {
  const char *id = (const char *)lv_event_get_user_data(e);
  if (!strcmp(id, "")) g_compose_deadline[0] = 0;
  else { strncpy(g_compose_deadline, id, sizeof(g_compose_deadline)-1);
         g_compose_deadline[sizeof(g_compose_deadline)-1] = 0; }
  build_compose_overlay();
}
static void on_rec_chip_clicked(lv_event_t *e) {
  g_compose_recurring = (bool)(intptr_t)lv_event_get_user_data(e);
  build_compose_overlay();
}

static void build_compose_overlay(void);

void open_compose_todo(void)  { g_compose_kind = COMPOSE_TODO;  build_compose_overlay(); }
void open_compose_habit(void) { g_compose_kind = COMPOSE_HABIT; build_compose_overlay(); }
void open_compose_event(void) { g_compose_kind = COMPOSE_EVENT; build_compose_overlay(); }

static void build_compose_overlay(void) {
  if (g_compose_overlay) {
    lv_obj_del(g_compose_overlay);
    g_compose_overlay = NULL;
  }
  g_compose_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(g_compose_overlay);
  lv_obj_set_size(g_compose_overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_compose_overlay, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_compose_overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flex_flow(g_compose_overlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_compose_overlay, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // -- Header: abbrechen / titel / sichern --
  lv_obj_t *hdr = lv_obj_create(g_compose_overlay);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_size(hdr, LV_PCT(100), 56);
  lv_obj_set_style_pad_left(hdr, 24, LV_PART_MAIN);
  lv_obj_set_style_pad_right(hdr, 24, LV_PART_MAIN);
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(hdr, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *cancel = lv_label_create(hdr);
  lv_label_set_text(cancel, "ABBRECHEN");
  lv_obj_set_style_text_font(cancel, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(cancel, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(cancel, 2, LV_PART_MAIN);
  lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(cancel, on_cancel_clicked, LV_EVENT_CLICKED, NULL);

  lv_obj_t *t = lv_label_create(hdr);
  lv_label_set_text(t,
      g_compose_kind == COMPOSE_TODO  ? "NEUE AUFGABE"
    : g_compose_kind == COMPOSE_HABIT ? "NEUE GEWOHNHEIT"
                                      : "NEUER TERMIN");
  lv_obj_set_style_text_font(t, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(t, 3, LV_PART_MAIN);

  lv_obj_t *save = lv_label_create(hdr);
  lv_label_set_text(save, "SICHERN");
  lv_obj_set_style_text_font(save, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(save, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(save, 2, LV_PART_MAIN);
  lv_obj_add_flag(save, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(save, on_save_clicked, LV_EVENT_CLICKED, NULL);

  // -- Body: title field + label "TITEL" --
  lv_obj_t *body = lv_obj_create(g_compose_overlay);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_style_pad_left(body, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(body, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(body, 24, LV_PART_MAIN);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 12, LV_PART_MAIN);

  lv_obj_t *kicker = lv_label_create(body);
  lv_label_set_text(kicker, "TITEL");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  g_compose_title_label = lv_label_create(body);
  lv_label_set_text(g_compose_title_label, g_compose_title[0] ? g_compose_title : "…");
  lv_obj_set_style_text_font(g_compose_title_label, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(g_compose_title_label, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_side(g_compose_title_label, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_compose_title_label, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_compose_title_label, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(g_compose_title_label, 6, LV_PART_MAIN);
  lv_obj_set_width(g_compose_title_label, LV_PCT(100));

  // -- Chips (todo only): category, deadline, recurring --
  if (g_compose_kind == COMPOSE_TODO) {
    auto add_label = [&](const char *t) {
      lv_obj_t *l = lv_label_create(body);
      lv_label_set_text(l, t);
      lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(l, lv_color_hex(MOKI_MID), LV_PART_MAIN);
      lv_obj_set_style_text_letter_space(l, 3, LV_PART_MAIN);
      lv_obj_set_style_pad_top(l, 4, LV_PART_MAIN);
    };
    auto chip_strip = [&](lv_obj_t *parent) {
      lv_obj_t *strip = lv_obj_create(parent);
      lv_obj_remove_style_all(strip);
      lv_obj_set_size(strip, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW_WRAP);
      lv_obj_set_style_pad_row(strip, 6, LV_PART_MAIN);
      lv_obj_set_style_pad_column(strip, 6, LV_PART_MAIN);
      return strip;
    };
    auto make_chip = [&](lv_obj_t *strip, const char *label, bool active,
                         lv_event_cb_t cb, const void *udata) {
      lv_obj_t *c = lv_obj_create(strip);
      lv_obj_remove_style_all(c);
      lv_obj_set_size(c, LV_SIZE_CONTENT, 38);
      lv_obj_set_style_bg_color(c, lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(c, lv_color_hex(active ? MOKI_INK : MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(c, 2, LV_PART_MAIN);
      lv_obj_set_style_pad_left(c, 12, LV_PART_MAIN);
      lv_obj_set_style_pad_right(c, 12, LV_PART_MAIN);
      lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, (void *)udata);
      lv_obj_t *l = lv_label_create(c);
      lv_label_set_text(l, label);
      lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(l,
          lv_color_hex(active ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
      lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
    };

    add_label("KATEGORIE");
    lv_obj_t *cs = chip_strip(body);
    static const char *CAT_IDS[]    = {"home","plants","work","self","social"};
    static const char *CAT_LABELS[] = {"zuhause","pflanzen","arbeit","selbst","freund_innen"};
    for (int i = 0; i < 5; i++)
      make_chip(cs, CAT_LABELS[i], !strcmp(g_compose_cat, CAT_IDS[i]),
                on_cat_chip_clicked, CAT_IDS[i]);

    add_label("BIS WANN");
    lv_obj_t *ds = chip_strip(body);
    static const char *DL_IDS[]    = {"heute","morgen","diese woche",""};
    static const char *DL_LABELS[] = {"heute","morgen","diese woche","ohne"};
    for (int i = 0; i < 4; i++)
      make_chip(ds, DL_LABELS[i], !strcmp(g_compose_deadline, DL_IDS[i]),
                on_dl_chip_clicked, DL_IDS[i]);

    add_label("WIEDERHOLT");
    lv_obj_t *rs = chip_strip(body);
    make_chip(rs, "einmalig",     !g_compose_recurring, on_rec_chip_clicked, (void *)0);
    make_chip(rs, "wöchentlich",   g_compose_recurring, on_rec_chip_clicked, (void *)1);
  }

  // -- Keyboard --
  lv_obj_t *kb = lv_obj_create(g_compose_overlay);
  lv_obj_remove_style_all(kb);
  lv_obj_set_size(kb, LV_PCT(100), 240);
  lv_obj_set_style_bg_color(kb, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
  lv_obj_set_style_border_color(kb, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(kb, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_top(kb, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(kb, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(kb, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(kb, 4, LV_PART_MAIN);

  static const char *row1[] = {"q","w","e","r","t","z","u","i","o","p"};
  static const char *row2[] = {"a","s","d","f","g","h","j","k","l","ä"};
  static const char *row3[] = {"y","x","c","v","b","n","m","ö","ü","ß"};
  build_keyboard_row(kb, row1, 10);
  build_keyboard_row(kb, row2, 10);
  build_keyboard_row(kb, row3, 10);

  // Bottom row — backspace + space
  lv_obj_t *r4 = lv_obj_create(kb);
  lv_obj_remove_style_all(r4);
  lv_obj_set_size(r4, LV_PCT(100), 56);
  lv_obj_set_flex_flow(r4, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r4, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r4, 6, LV_PART_MAIN);

  lv_obj_t *back = lv_obj_create(r4);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, 110, 50);
  lv_obj_set_style_bg_color(back, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(back, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_border_width(back, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(back, 2, LV_PART_MAIN);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_key_clicked, LV_EVENT_CLICKED, (void *)"BACK");
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← BACK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 1, LV_PART_MAIN);
  lv_obj_center(bl);

  lv_obj_t *space = lv_obj_create(r4);
  lv_obj_remove_style_all(space);
  lv_obj_set_size(space, 280, 50);
  lv_obj_set_style_bg_color(space, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(space, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(space, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_border_width(space, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(space, 2, LV_PART_MAIN);
  lv_obj_add_flag(space, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(space, on_key_clicked, LV_EVENT_CLICKED, (void *)"SPACE");
  lv_obj_t *sl = lv_label_create(space);
  lv_label_set_text(sl, "LEERZEICHEN");
  lv_obj_set_style_text_font(sl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sl, 2, LV_PART_MAIN);
  lv_obj_center(sl);
}

#define DISP_BUF_SIZE (epd_rotated_display_width() * epd_rotated_display_height())

// LVGL plumbing — render-then-flush pattern from LILYGO lvgl_test, kept as-is
static uint8_t       *decodebuffer        = NULL;
static lv_timer_t    *flush_timer         = NULL;
static volatile bool  disp_flush_enabled  = true;

static inline void check_err(enum EpdDrawError err) {
  if (err != EPD_DRAW_SUCCESS) {
    Serial.printf("[epd] draw error: %X\n", err);
  }
}

// LVGL flush callback — converts each LVGL 32-bit pixel to a 4-bit greyscale
// value (luminance) and writes it into a 4-bit-packed buffer (two pixels per
// byte: low nibble = even x, high nibble = odd x).
// Track the dirty area LVGL hands us — flush_timer_cb uses this to do a
// PARTIAL EPD update instead of the full 540×960. For typing/scrolling a
// label, the dirty rect is often <50px tall — 10× smaller area means 10×
// less data through the EPD waveform = visibly snappier updates.
static volatile int g_dirty_x = 0, g_dirty_y = 0, g_dirty_w = 0, g_dirty_h = 0;

static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  if (disp_flush_enabled) {
    uint16_t       w   = lv_area_get_width(area);
    uint16_t       h   = lv_area_get_height(area);
    lv_color32_t  *t32 = (lv_color32_t *)color_p;

    for (int i = 0; i < (w * h); i++) {
      uint8_t r = LV_COLOR_GET_R(*t32);
      uint8_t g = LV_COLOR_GET_G(*t32);
      uint8_t b = LV_COLOR_GET_B(*t32);
      uint16_t y8 = (r * 299 + g * 587 + b * 114) / 1000;
      if      (y8 <  80) y8 = 0;
      else if (y8 > 175) y8 = 255;
      uint8_t  gray4 = (uint8_t)(y8 >> 4) & 0x0F;

      int byte_idx = i / 2;
      if (i & 1) {
        decodebuffer[byte_idx] |= gray4 << 4;
      } else {
        decodebuffer[byte_idx]  = gray4;
      }
      t32++;
    }

    // Push this chunk into the framebuffer at its actual position. Multiple
    // chunks per render union into a single dirty rect for the EPD update.
    EpdRect rect = { .x = area->x1, .y = area->y1, .width = w, .height = h };
    epd_draw_rotated_image(rect, decodebuffer, epd_hl_get_framebuffer(&hl));

    // Union with any previous chunk(s) in this same render cycle.
    if (g_dirty_w == 0) {
      g_dirty_x = rect.x; g_dirty_y = rect.y;
      g_dirty_w = rect.width; g_dirty_h = rect.height;
    } else {
      int x2 = (rect.x + rect.width)  > (g_dirty_x + g_dirty_w)  ? rect.x + rect.width  : g_dirty_x + g_dirty_w;
      int y2 = (rect.y + rect.height) > (g_dirty_y + g_dirty_h)  ? rect.y + rect.height : g_dirty_y + g_dirty_h;
      if (rect.x < g_dirty_x) g_dirty_x = rect.x;
      if (rect.y < g_dirty_y) g_dirty_y = rect.y;
      g_dirty_w = x2 - g_dirty_x;
      g_dirty_h = y2 - g_dirty_y;
    }
  }
  lv_disp_flush_ready(disp);
}

static void flush_timer_cb(lv_timer_t *t) {
  // Bail out if no chunks were captured (defensive — shouldn't happen).
  if (g_dirty_w == 0 || g_dirty_h == 0) {
    lv_timer_pause(flush_timer);
    return;
  }

  // Use the union of all chunks captured in disp_flush as the EPD update area.
  // Massive win over hardcoded full-screen: typing a key only updates ~50px tall.
  EpdRect dirty = {
    .x = g_dirty_x, .y = g_dirty_y,
    .width = g_dirty_w, .height = g_dirty_h
  };

  epd_poweron();
  // MODE_DU (1-bit, ~150ms for full screen — proportionally less for partials)
  // is fine because disp_flush threshold-snaps to pure black/white. Every 8th
  // flush we run a MODE_GC16 full-screen pass to flush ghosting.
  static uint32_t flush_count = 0;
  bool full_refresh = ((flush_count & 7) == 0);
  if (full_refresh) {
    check_err(epd_hl_update_screen(&hl, MODE_GC16, epd_ambient_temperature()));
    Serial.println(F("[epd] full GC16 cleanup"));
  } else {
    check_err(epd_hl_update_area(&hl, MODE_DU, epd_ambient_temperature(), dirty));
  }
  epd_poweroff();
  flush_count++;

  // Reset dirty rect for the next render cycle.
  g_dirty_x = g_dirty_y = g_dirty_w = g_dirty_h = 0;

  lv_timer_pause(flush_timer);
}

static void disp_render_start_cb(struct _lv_disp_drv_t *disp_drv) {
  // Push to EPD ASAP after LVGL finishes rendering. Original 200ms timer
  // period silently added 0..200ms latency to every interactive update.
  // 30ms is the minimum we need to coalesce tightly-coupled invalidations
  // (e.g., during a single LVGL refresh cycle) without throwing away
  // typing snappiness.
  if (flush_timer == NULL) {
    flush_timer = lv_timer_create(flush_timer_cb, 30, NULL);
  }
  lv_timer_resume(flush_timer);
  lv_timer_ready(flush_timer);   // make it fire on the very next lv_timer_handler tick
}

static void lv_port_disp_init(void) {
  lv_init();

  static lv_disp_draw_buf_t draw_buf;

  lv_color_t *buf1 = (lv_color_t *)ps_calloc(sizeof(lv_color_t), DISP_BUF_SIZE);
  lv_color_t *buf2 = (lv_color_t *)ps_calloc(sizeof(lv_color_t), DISP_BUF_SIZE);
  // 4-bit packed greyscale → DISP_BUF_SIZE / 2 bytes (2 pixels per byte).
  decodebuffer    = (uint8_t   *)ps_calloc(sizeof(uint8_t),    DISP_BUF_SIZE / 2);

  if (!buf1 || !buf2 || !decodebuffer) {
    Serial.println(F("[lvgl] PSRAM alloc FAILED — display will not render"));
    return;
  }

  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, DISP_BUF_SIZE);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res          = epd_rotated_display_width();
  disp_drv.ver_res          = epd_rotated_display_height();
  disp_drv.flush_cb         = disp_flush;
  disp_drv.render_start_cb  = disp_render_start_cb;
  disp_drv.draw_buf         = &draw_buf;
  disp_drv.full_refresh     = 1;
  lv_disp_drv_register(&disp_drv);
}

// ----------------------------------------------------------------------------
// Synthetic touch — driven by Serial commands so a host can drive UI tests.
// 'tap X Y' / 'long X Y' set these volatiles; loop() releases on a timer.
// ----------------------------------------------------------------------------
static volatile bool    synth_pressed = false;
static volatile int16_t synth_x       = 0;
static volatile int16_t synth_y       = 0;
static volatile uint32_t synth_release_at_ms = 0;

static void synth_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  data->state   = synth_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = synth_x;
  data->point.y = synth_y;
}

// LVGL indev — read touch state from GT911. GT911 native is panel-landscape
// (960×540); our display rotation is INVERTED_PORTRAIT (540×960). The mapping
// is a 270° rotation between native and rotated coordinate frames.
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  static bool was_down = false;
  int16_t tx[5], ty[5];
  if (touch.isPressed()) {
    uint8_t n = touch.getPoint(tx, ty, 1);
    if (n > 0) {
      int16_t raw_x = tx[0];
      int16_t raw_y = ty[0];
      // GT911 native frame already matches our INVERTED_PORTRAIT LVGL coords
      // (540 × 960). No rotation needed — the swap I had in Stage 1c was
      // accidental: it pushed coords out of bounds but happened to still
      // hit a fullscreen-clickable handler.
      int16_t lx = raw_x;
      int16_t ly = raw_y;
      // Edge-trigger diagnostic — print only the first sample of each press.
      if (!was_down) {
        Serial.printf("[touch] press raw(%d,%d) → lvgl(%d,%d)\n",
                      raw_x, raw_y, lx, ly);
        was_down = true;
      }
      data->state   = LV_INDEV_STATE_PRESSED;
      data->point.x = lx;
      data->point.y = ly;
      return;
    }
  }
  was_down = false;
  data->state = LV_INDEV_STATE_RELEASED;
}

// ----------------------------------------------------------------------------
// Tap diagnostics — every interactive element prints to Serial when tapped so
// we can validate hit-testing without wiring real navigation yet.
// ----------------------------------------------------------------------------
static void on_element_tapped(lv_event_t *e) {
  const char *name = (const char *)lv_event_get_user_data(e);
  Serial.printf("[tap] %s\n", name);
}

// LVGL 8.3 has no margin styles, so we drop in transparent spacers between
// flex children to add vertical breathing room.
static void add_spacer(lv_obj_t *parent, int height) {
  lv_obj_t *sp = lv_obj_create(parent);
  lv_obj_remove_style_all(sp);
  lv_obj_set_size(sp, 1, height);
  lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
}

// ----------------------------------------------------------------------------
// Moki pet — translated from simulator.jsx lines 270-365. SVG viewBox is
// 0..100; we scale by 2.0 onto a 200×200 canvas. Ellipses become rounded
// rectangles with LV_RADIUS_CIRCLE (LVGL 8.3 has no native ellipse).
//
// E-Ink palette tweak: belly uses MID (not DARK) — DARK snaps to pure black
// under the disp_flush threshold, which makes it disappear into the INK body.
// MID stays in the mid-grey range and reads as a subtle accent. Shadow is
// skipped (LIGHT also snaps to white and was invisible).
// ----------------------------------------------------------------------------
#define MOKI_PET_SIZE 200
static lv_color_t moki_pet_buf[MOKI_PET_SIZE * MOKI_PET_SIZE];

static lv_obj_t *create_moki_canvas(lv_obj_t *parent) {
  lv_obj_t *cv = lv_canvas_create(parent);
  lv_canvas_set_buffer(cv, moki_pet_buf, MOKI_PET_SIZE, MOKI_PET_SIZE,
                       LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(cv, lv_color_hex(MOKI_PAPER), LV_OPA_COVER);

  // ---- Ears (tufted variant) ----
  lv_draw_rect_dsc_t ear;
  lv_draw_rect_dsc_init(&ear);
  ear.bg_color = lv_color_hex(MOKI_INK);
  ear.bg_opa   = LV_OPA_COVER;

  // Left ear : Bezier path "M 28 42 Q 24 24 34 26 Q 36 36 36 44 Z" → polygon
  lv_point_t ear_left[] = { {56,84}, {48,48}, {68,52}, {72,88} };
  lv_canvas_draw_polygon(cv, ear_left, 4, &ear);

  // Right ear : mirrored
  lv_point_t ear_right[] = { {144,84}, {152,48}, {132,52}, {128,88} };
  lv_canvas_draw_polygon(cv, ear_right, 4, &ear);

  // ---- Body (ellipse cx=50 cy=62 rx=28 ry=26, fill=INK) ----
  lv_draw_rect_dsc_t body;
  lv_draw_rect_dsc_init(&body);
  body.bg_color = lv_color_hex(MOKI_INK);
  body.bg_opa   = LV_OPA_COVER;
  body.radius   = LV_RADIUS_CIRCLE;
  lv_canvas_draw_rect(cv, 44, 72, 112, 104, &body);

  // Belly skipped — under the disp_flush threshold any color other than INK
  // reads as a "hole" cut into the body. Custom fonts come first (Stage 2c),
  // we revisit the belly when we have alpha-blending working.

  // ---- Eyes (mood=calm: PAPER dots) ----
  lv_draw_rect_dsc_t eye;
  lv_draw_rect_dsc_init(&eye);
  eye.bg_color = lv_color_hex(MOKI_PAPER);
  eye.bg_opa   = LV_OPA_COVER;
  eye.radius   = LV_RADIUS_CIRCLE;
  lv_canvas_draw_rect(cv,  80, 114, 10, 10, &eye);
  lv_canvas_draw_rect(cv, 110, 114, 10, 10, &eye);

  // ---- Mouth (path "M 47 70 Q 50 72 53 70" → polyline in PAPER) ----
  lv_draw_line_dsc_t mouth;
  lv_draw_line_dsc_init(&mouth);
  mouth.color = lv_color_hex(MOKI_PAPER);
  mouth.width = 3;
  mouth.round_start = 1;
  mouth.round_end   = 1;
  lv_point_t mp[] = { {94,140}, {100,144}, {106,140} };
  lv_canvas_draw_line(cv, mp, 3, &mouth);

  // ---- Paws — small filled rounded shapes at the body bottom ----
  lv_draw_rect_dsc_t paw;
  lv_draw_rect_dsc_init(&paw);
  paw.bg_color = lv_color_hex(MOKI_INK);
  paw.bg_opa   = LV_OPA_COVER;
  paw.radius   = 7;            // half-height, rounds the corners cleanly
  lv_canvas_draw_rect(cv,  68, 168, 26, 14, &paw);
  lv_canvas_draw_rect(cv, 106, 168, 26, 14, &paw);

  // ---- Subtle ground shadow under the body ----
  lv_draw_rect_dsc_t shadow;
  lv_draw_rect_dsc_init(&shadow);
  shadow.bg_color = lv_color_hex(0x6F6F6F);   // mid-grey, survives threshold
  shadow.bg_opa   = LV_OPA_COVER;
  shadow.radius   = 4;
  lv_canvas_draw_rect(cv, 56, 184, 88, 6, &shadow);

  return cv;
}

// ----------------------------------------------------------------------------
// build_status_bar — top strip: sync indicator left, battery + time right,
// dashed bottom border per design DNA.
// ----------------------------------------------------------------------------
static lv_obj_t *build_status_bar(lv_obj_t *parent) {
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LV_PCT(100), 48);
  lv_obj_set_style_bg_color(bar, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(bar, 24, LV_PART_MAIN);
  lv_obj_set_style_pad_right(bar, 24, LV_PART_MAIN);
  lv_obj_set_style_pad_top(bar, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(bar, 4, LV_PART_MAIN);
  lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(bar, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Sync countdown derived from uptime + configured interval.
  uint32_t uptime_min = millis() / 60000UL;
  uint32_t since      = uptime_min % g_settings.sync_interval_min;
  uint32_t left       = g_settings.sync_interval_min - since;
  char syncbuf[24];
  snprintf(syncbuf, sizeof(syncbuf), "SYNC · %luM", (unsigned long)left);
  lv_obj_t *sync = lv_label_create(bar);
  lv_label_set_text(sync, syncbuf);
  lv_obj_set_style_text_color(sync, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_font(sync, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sync, 2, LV_PART_MAIN);
  lv_obj_add_flag(sync, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(sync, on_element_tapped, LV_EVENT_CLICKED, (void *)"sync");

  // Right cluster — battery placeholder + WiFi state + real time
#ifdef MOKI_WIFI
  const char *wifi_glyph = g_wifi.connected ? " W " : (g_wifi.enabled ? " w " : "   ");
#else
  const char *wifi_glyph = "   ";
#endif
  char rightbuf[40];
  if (g_rtc_ready) {
    RTC_DateTime now = g_rtc.getDateTime();
    snprintf(rightbuf, sizeof(rightbuf), "78%s%02u:%02u",
             wifi_glyph, now.hour, now.minute);
  } else {
    uint32_t hh = uptime_min / 60;
    uint32_t mm = uptime_min % 60;
    snprintf(rightbuf, sizeof(rightbuf), "78%s%02lu:%02lu",
             wifi_glyph, (unsigned long)(hh % 24), (unsigned long)mm);
  }
  lv_obj_t *right = lv_label_create(bar);
  lv_label_set_text(right, rightbuf);
  lv_obj_set_style_text_color(right, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_font(right, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(right, 2, LV_PART_MAIN);

  return bar;
}

// ----------------------------------------------------------------------------
// build_dock — bottom 5-slot navigation: heim · tun · lesen · chat · karte.
// Active item gets a 1.5px INK underline below its label.
// ----------------------------------------------------------------------------
static lv_obj_t *build_dock(lv_obj_t *parent, int active_idx) {
  static const char *items[] = {"heim", "tun", "lesen", "chat", "karte"};

  lv_obj_t *dock = lv_obj_create(parent);
  lv_obj_remove_style_all(dock);
  lv_obj_set_size(dock, LV_PCT(100), 76);
  lv_obj_set_style_bg_color(dock, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_side(dock, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
  lv_obj_set_style_border_width(dock, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(dock, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_AROUND,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (int i = 0; i < 5; i++) {
    lv_obj_t *item = lv_obj_create(dock);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, 100, 64);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(item, on_dock_clicked,
                        LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *lbl = lv_label_create(item);
    lv_label_set_text(lbl, items[i]);
    lv_obj_set_style_text_font(lbl, &moki_jetbrains_mono_28, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(lbl, 1, LV_PART_MAIN);
    bool active = (i == active_idx);
    lv_obj_set_style_text_color(lbl,
        lv_color_hex(active ? MOKI_INK : MOKI_DARK), LV_PART_MAIN);

    if (active) {
      add_spacer(item, 4);
      // Underline the active item — 1.5px solid INK below the label.
      lv_obj_t *underline = lv_obj_create(item);
      lv_obj_remove_style_all(underline);
      lv_obj_set_size(underline, 28, 2);
      lv_obj_set_style_bg_color(underline, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(underline, LV_OPA_COVER, LV_PART_MAIN);
    }
  }

  return dock;
}

// ----------------------------------------------------------------------------
// build_home_content — the central column: date, title, pet, mood pill, tiles.
// ----------------------------------------------------------------------------
static void build_stat_tile(lv_obj_t *parent, const char *kicker,
                            const char *value, const char *sub,
                            const char *tap_id) {
  lv_obj_t *tile = lv_obj_create(parent);
  lv_obj_remove_style_all(tile);
  lv_obj_set_size(tile, LV_PCT(31), 110);
  lv_obj_set_style_bg_color(tile, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(tile, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(tile, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(tile, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  static auto on_stat_cb = [](lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    if (!strcmp(id, "stat-gewohn")) { current_do_tab = DO_HABITS; switch_screen(SCR_DO); }
    else if (!strcmp(id, "stat-aufgab")) { current_do_tab = DO_TODOS;  switch_screen(SCR_DO); }
    else if (!strcmp(id, "stat-nah"))    { current_map_tab = MAP_NEARBY; switch_screen(SCR_MAP); }
  };
  lv_obj_add_event_cb(tile, on_stat_cb,
                      LV_EVENT_CLICKED, (void *)tap_id);

  lv_obj_t *k = lv_label_create(tile);
  lv_label_set_text(k, kicker);
  lv_obj_set_style_text_font(k, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(k, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(k, 2, LV_PART_MAIN);

  lv_obj_t *v = lv_label_create(tile);
  lv_label_set_text(v, value);
  lv_obj_set_style_text_font(v, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(v, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  lv_obj_t *s = lv_label_create(tile);
  lv_label_set_text(s, sub);
  lv_obj_set_style_text_font(s, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(s, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(s, 1, LV_PART_MAIN);
}

static void build_home_content(lv_obj_t *parent) {
  lv_obj_t *col = lv_obj_create(parent);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(col, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(col, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(col, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(col, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(col, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(col, 12, LV_PART_MAIN);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  // SPACE_AROUND distributes children evenly across the available height —
  // no clustering at the top, no big empty stretch above the dock.
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_SPACE_AROUND,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_flex_grow(col, 1);

  // -- Top row: date kicker (left) + profile circle (right) --
  lv_obj_t *top = lv_obj_create(col);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, LV_PCT(100), 44);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *kicker = lv_label_create(top);
  lv_label_set_text(kicker, "DIENSTAG · 20. APRIL");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  lv_obj_t *prof = lv_obj_create(top);
  lv_obj_remove_style_all(prof);
  lv_obj_set_size(prof, 44, 44);
  lv_obj_set_style_border_color(prof, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(prof, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(prof, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_flex_flow(prof, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(prof, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(prof, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(prof, on_profile_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_t *pl = lv_label_create(prof);
  lv_label_set_text(pl, "L");
  lv_obj_set_style_text_font(pl, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(pl, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  // -- Title (italic-ish, will become Fraunces in 2c) --
  lv_obj_t *title = lv_label_create(col);
  lv_label_set_text(title, "langsam, aber jeden tag.");
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  // -- Moki pet — vector drawing via lv_canvas (Stage 2b) --
  lv_obj_t *pet = create_moki_canvas(col);
  lv_obj_set_size(pet, MOKI_PET_SIZE, MOKI_PET_SIZE);
  lv_obj_add_flag(pet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(pet, on_element_tapped, LV_EVENT_CLICKED, (void *)"moki");

  // -- Pet name + meta as a single tight pair (their own column) --
  lv_obj_t *pet_pair = lv_obj_create(col);
  lv_obj_remove_style_all(pet_pair);
  lv_obj_set_size(pet_pair, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(pet_pair, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(pet_pair, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(pet_pair, 4, LV_PART_MAIN);

  lv_obj_t *pet_name = lv_label_create(pet_pair);
  lv_label_set_text(pet_name, "moki");
  lv_obj_set_style_text_font(pet_name, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(pet_name, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  lv_obj_t *pet_meta = lv_label_create(pet_pair);
  lv_label_set_text(pet_meta, "TAG 14 · 3 IN FOLGE");
  lv_obj_set_style_text_font(pet_meta, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(pet_meta, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(pet_meta, 2, LV_PART_MAIN);

  // -- Mood pill — shows active state, opens picker on tap --
  bool mood_active = (g_active_mood[0] != 0);
  const moki_mood_def_t *active_def = NULL;
  if (mood_active) {
    for (int i = 0; i < MOOD_COUNT; i++) {
      if (!strcmp(MOOD_PRESETS[i].id, g_active_mood)) { active_def = &MOOD_PRESETS[i]; break; }
    }
  }

  lv_obj_t *mood = lv_obj_create(col);
  lv_obj_remove_style_all(mood);
  lv_obj_set_size(mood, LV_PCT(100), 72);
  lv_obj_set_style_bg_color(mood,
      lv_color_hex(mood_active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(mood, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(mood, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_border_width(mood, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(mood, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_left(mood, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_right(mood, 14, LV_PART_MAIN);
  lv_obj_set_flex_flow(mood, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(mood, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(mood, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(mood, on_mood_pill_clicked, LV_EVENT_CLICKED, NULL);

  lv_obj_t *mq = lv_label_create(mood);
  if (mood_active && active_def) {
    char active_line[64];
    snprintf(active_line, sizeof(active_line), "du teilst: %s", active_def->label);
    lv_label_set_text(mq, active_line);
  } else {
    lv_label_set_text(mq, "wie fühlst du dich heute?");
  }
  lv_obj_set_style_text_font(mq, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(mq,
      lv_color_hex(mood_active ? MOKI_PAPER : MOKI_DARK), LV_PART_MAIN);

  lv_obj_t *ma = lv_label_create(mood);
  lv_label_set_text(ma, mood_active ? "AKTIV" : "TEILEN →");
  lv_obj_set_style_text_font(ma, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(ma,
      lv_color_hex(mood_active ? MOKI_PAPER : MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(ma, 2, LV_PART_MAIN);

  // -- Three stat tiles --
  lv_obj_t *tiles = lv_obj_create(col);
  lv_obj_remove_style_all(tiles);
  lv_obj_set_size(tiles, LV_PCT(100), 115);
  lv_obj_set_style_bg_opa(tiles, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  // Live counts
  int habits_done = 0, todos_open = 0;
  for (int i = 0; i < g_habits_count; i++) if (g_habits[i].today_count > 0) habits_done++;
  for (int i = 0; i < g_todos_count; i++)  if (!g_todos[i].done) todos_open++;
  char gewohn_v[16], aufgab_v[16], nah_v[16];
  snprintf(gewohn_v, sizeof(gewohn_v), "%d/%d", habits_done, g_habits_count);
  snprintf(aufgab_v, sizeof(aufgab_v), "%d", todos_open);
  snprintf(nah_v,    sizeof(nah_v),    "%d", SAMPLE_NEARBY_COUNT);

  build_stat_tile(tiles, "GEWOHN", gewohn_v, "heute",       "stat-gewohn");
  build_stat_tile(tiles, "AUFGAB", aufgab_v, "offen",       "stat-aufgab");
  build_stat_tile(tiles, "NAH",    nah_v,    "in der nähe", "stat-nah");
}

// ----------------------------------------------------------------------------
// build_screen_chrome — paints the screen background, status-bar at top, dock
// at bottom. Returns the middle content container (LV_PCT 100/grow=1) for
// the per-screen builder to fill.
// ----------------------------------------------------------------------------
static lv_obj_t *build_screen_chrome(lv_obj_t *scr, int active_dock_idx) {
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  build_status_bar(scr);

  lv_obj_t *content = lv_obj_create(scr);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(content, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(content, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flex_grow(content, 1);

  build_dock(scr, active_dock_idx);

  return content;
}

// ----------------------------------------------------------------------------
// switch_screen — clears current and rebuilds the requested one.
// ----------------------------------------------------------------------------
// Recursively turn off elastic + momentum scrolling on every scrollable
// child of `obj`. E-Ink can't render motion smoothly — every overshoot or
// inertial-fling animation forces partial-refreshes that pile up and cause
// a full-clear flash. Direct 1:1 finger-tracking + instant stop is the
// only thing that feels right on this display.
static void eink_tune_scroll(lv_obj_t *obj) {
  if (!obj) return;
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
  uint32_t n = lv_obj_get_child_cnt(obj);
  for (uint32_t i = 0; i < n; i++) {
    eink_tune_scroll(lv_obj_get_child(obj, i));
  }
}

static void switch_screen(screen_id_t to) {
  lv_obj_clean(lv_scr_act());
  current_screen = to;
  switch (to) {
    case SCR_HOME:    build_home();    break;
    case SCR_DO:      build_do();      break;
    case SCR_READ:    build_read();    break;
    case SCR_CHAT:    build_chats();   break;
    case SCR_MAP:     build_map();     break;
    case SCR_MOOD:     build_mood();     break;
    case SCR_PROFILE:  build_profile();  break;
    case SCR_NOTE_NEW: build_note_new(); break;
    case SCR_NOTE_EDIT:build_note_edit();break;
    case SCR_CHAT_DETAIL: { extern void build_chat_detail(void); build_chat_detail(); break; }
    case SCR_READER:      { extern void build_reader(void);      build_reader();      break; }
    case SCR_PIN_DETAIL:  { extern void build_pin_detail(void);  build_pin_detail();  break; }
    case SCR_PIN_RENAME:  { extern void build_pin_rename(void);  build_pin_rename();  break; }
    case SCR_PIN_NOTE:    { extern void build_pin_note(void);    build_pin_note();    break; }
    case SCR_WIFI:        { extern void build_wifi(void);        build_wifi();        break; }
    case SCR_WIFI_PSK:    { extern void build_wifi_psk(void);    build_wifi_psk();    break; }
    case SCR_OTA:         { extern void build_ota_screen(void);  build_ota_screen();  break; }
    case SCR_SETTINGS:    { extern void build_settings(void);    build_settings();    break; }
  }
  // After building, disable elastic + momentum scroll on the whole tree.
  eink_tune_scroll(lv_scr_act());
}

// ----------------------------------------------------------------------------
// Toast — bottom overlay that fades in via partial epd update.
// Simple LVGL approach: an obj on lv_layer_top() with an auto-delete timer.
// ----------------------------------------------------------------------------
static lv_obj_t *g_toast = NULL;
static lv_timer_t *g_toast_timer = NULL;
static void toast_kill_cb(lv_timer_t *t) {
  if (g_toast) { lv_obj_del(g_toast); g_toast = NULL; }
  if (g_toast_timer) { lv_timer_del(g_toast_timer); g_toast_timer = NULL; }
}
static void show_toast(const char *text) {
  if (g_toast) { lv_obj_del(g_toast); g_toast = NULL; }
  if (g_toast_timer) { lv_timer_del(g_toast_timer); g_toast_timer = NULL; }

  g_toast = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(g_toast);
  lv_obj_set_size(g_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(g_toast, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(g_toast, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g_toast, 12, LV_PART_MAIN);
  lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -100);
  lv_obj_t *l = lv_label_create(g_toast);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(l, 2, LV_PART_MAIN);
  g_toast_timer = lv_timer_create(toast_kill_cb, 2500, NULL);
  lv_timer_set_repeat_count(g_toast_timer, 1);
}

int g_active_chat = -1;   // legacy mockup-list index; -1 means new-style chat target

// New-style chat targeting (Lucas's WhatsApp/Telegram pattern).
// kind: 0 = MeshCore channel @ idx, 1 = DM with contact @ idx
// When set, build_chat_detail filters by these and ignores g_active_chat.
int g_chat_target_kind = -1;   // -1 = none yet, 0 = channel, 1 = dm
int g_chat_target_idx  = 0;
static void on_chat_open(lv_event_t *e) {
  g_active_chat = (int)(intptr_t)lv_event_get_user_data(e);
  switch_screen(SCR_CHAT_DETAIL);
}
static void on_chat_back(lv_event_t *e) {
  g_active_chat = -1;
  g_chat_target_kind = -1;
  switch_screen(SCR_CHAT);
}

static void on_mood_pill_clicked(lv_event_t *e) {
  switch_screen(SCR_MOOD);
}
static void on_mood_picked(lv_event_t *e) {
  const char *id = (const char *)lv_event_get_user_data(e);
  strncpy(g_active_mood, id, sizeof(g_active_mood)-1);
  g_active_mood[sizeof(g_active_mood)-1] = 0;
  state_save_mood();
  switch_screen(SCR_HOME);
  show_toast("STIMMUNG GETEILT");
}
static void on_mood_clear(lv_event_t *e) {
  g_active_mood[0] = 0;
  state_save_mood();
  switch_screen(SCR_HOME);
}
static void on_back_home(lv_event_t *e) {
  switch_screen(SCR_HOME);
}
static void on_profile_clicked(lv_event_t *e) {
  switch_screen(SCR_PROFILE);
}

// ----------------------------------------------------------------------------
// HOME screen — date kicker, italic title, pet, mood pill, three stat tiles.
// ----------------------------------------------------------------------------
static void build_home(void) {
  lv_obj_t *content = build_screen_chrome(lv_scr_act(), 0);
  build_home_content(content);
}

// ----------------------------------------------------------------------------
// Reusable helpers for screens with tabs
// ----------------------------------------------------------------------------
static void make_tab_button(lv_obj_t *parent, const char *label, bool active,
                            lv_event_cb_t cb, intptr_t tab_idx) {
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_flex_grow(btn, 1);
  lv_obj_set_height(btn, 50);
  lv_obj_set_style_bg_color(btn, lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(btn, 1, LV_PART_MAIN);
  lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)tab_idx);

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_font(lbl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl,
      lv_color_hex(active ? MOKI_PAPER : MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lbl, 2, LV_PART_MAIN);
}

static lv_obj_t *make_tab_bar(lv_obj_t *parent) {
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(bar, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(bar, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(bar, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  return bar;
}

// ----------------------------------------------------------------------------
// DO screen — gewohnheiten | aufgaben | kalender
// ----------------------------------------------------------------------------
static void on_todo_toggle(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_todos_count) return;
  g_todos[idx].done = !g_todos[idx].done;
  Serial.printf("[todo] toggle #%d → done=%d\n", idx, g_todos[idx].done);
  state_save_todos();
  switch_screen(SCR_DO);
}

static int g_active_habit = -1;                  // selected habit for detail view
static void build_habit_detail(void);

static void on_habit_increment(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_habits_count) return;
  moki_habit_t *h = &g_habits[idx];
  if (h->today_count == 0) h->streak += 1;       // first count of today
  if (h->today_count < 99) {
    h->today_count += 1;
    h->history[83]  = h->today_count;
  }
  Serial.printf("[habit] +1 #%d → today=%d streak=%d\n", idx, h->today_count, h->streak);
  state_save_habits();
  // Re-render whichever screen we're on (DO list or detail)
  if (g_active_habit == idx) build_habit_detail();
  else                       switch_screen(SCR_DO);
}

static void on_habit_decrement(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_habits_count) return;
  moki_habit_t *h = &g_habits[idx];
  if (h->today_count > 0) {
    h->today_count -= 1;
    h->history[83]  = h->today_count;
    if (h->today_count == 0 && h->streak > 0) h->streak -= 1;  // undo the 0→1 streak bump
  }
  Serial.printf("[habit] -1 #%d → today=%d streak=%d\n", idx, h->today_count, h->streak);
  state_save_habits();
  if (g_active_habit == idx) build_habit_detail();
  else                       switch_screen(SCR_DO);
}

static void on_habit_open_detail(lv_event_t *e) {
  g_active_habit = (int)(intptr_t)lv_event_get_user_data(e);
  build_habit_detail();
}

static void on_habit_back(lv_event_t *e) {
  g_active_habit = -1;
  switch_screen(SCR_DO);
}

static void on_habit_delete(lv_event_t *e) {
  if (g_active_habit < 0 || g_active_habit >= g_habits_count) return;
  Serial.printf("[habit] delete #%d '%s'\n", g_active_habit, g_habits[g_active_habit].name);
  for (int i = g_active_habit; i < g_habits_count - 1; i++)
    g_habits[i] = g_habits[i+1];
  g_habits_count--;
  state_save_habits();
  show_toast("GEWOHNHEIT GELÖSCHT");
  g_active_habit = -1;
  current_do_tab = DO_HABITS;
  switch_screen(SCR_DO);
}

static void build_todo_row(lv_obj_t *parent, int idx) {
  const moki_todo_t *t = &g_todos[idx];
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_top(row, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(row, 8, LV_PART_MAIN);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(row, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);

  // Checkbox — tap toggles done state
  lv_obj_t *cb = lv_obj_create(row);
  lv_obj_remove_style_all(cb);
  lv_obj_set_size(cb, 32, 32);                     // bigger hit-target
  lv_obj_set_style_border_color(cb, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(cb, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(cb, lv_color_hex(t->done ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(cb, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(cb, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(cb, on_todo_toggle, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
  if (t->done) {
    lv_obj_t *check = lv_label_create(cb);
    lv_label_set_text(check, "✓");
    lv_obj_set_style_text_color(check, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_text_font(check, &moki_fraunces_regular_36, LV_PART_MAIN);
    lv_obj_center(check);
  }

  // Text column
  lv_obj_t *col = lv_obj_create(row);
  lv_obj_remove_style_all(col);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *title = lv_label_create(col);
  lv_label_set_text(title, t->title);
  lv_obj_set_style_text_font(title, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title,
      lv_color_hex(t->done ? MOKI_DARK : MOKI_INK), LV_PART_MAIN);

  // Meta line: cat-mark + cat-label + bis deadline + ↻ wiederkehrend
  char meta[96];
  snprintf(meta, sizeof(meta), "%s %s%s%s%s",
           cat_mark(t->cat), cat_label(t->cat),
           (t->deadline && t->deadline[0]) ? "  ·  bis " : "",
           (t->deadline && t->deadline[0]) ? t->deadline : "",
           t->recurring ? "  ·  ↻ wöchentlich" : "");
  lv_obj_t *m = lv_label_create(col);
  lv_label_set_text(m, meta);
  lv_obj_set_style_text_font(m, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(m, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(m, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_top(m, 3, LV_PART_MAIN);
}

static void build_do_content(lv_obj_t *parent) {
  lv_obj_t *col = lv_obj_create(parent);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_left(col, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(col, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(col, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(col, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(col, 12, LV_PART_MAIN);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_grow(col, 1);

  // Tab bar
  lv_obj_t *bar = make_tab_bar(col);
  make_tab_button(bar, "gewohnheiten", current_do_tab == DO_HABITS,   on_do_tab_clicked, DO_HABITS);
  make_tab_button(bar, "aufgaben",     current_do_tab == DO_TODOS,    on_do_tab_clicked, DO_TODOS);
  make_tab_button(bar, "kalender",     current_do_tab == DO_CALENDAR, on_do_tab_clicked, DO_CALENDAR);

  // Tab content
  lv_obj_t *body = lv_obj_create(col);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 0, LV_PART_MAIN);

  if (current_do_tab == DO_TODOS) {
    // Open todos first, then "ERLEDIGT" header, then done todos
    bool printed_done_header = false;
    for (int i = 0; i < g_todos_count; i++) {
      if (!g_todos[i].done) build_todo_row(body, i);
    }
    for (int i = 0; i < g_todos_count; i++) {
      if (g_todos[i].done) {
        if (!printed_done_header) {
          lv_obj_t *hdr = lv_label_create(body);
          lv_label_set_text(hdr, "ERLEDIGT");
          lv_obj_set_style_text_font(hdr, &moki_jetbrains_mono_22, LV_PART_MAIN);
          lv_obj_set_style_text_color(hdr, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
          lv_obj_set_style_text_letter_space(hdr, 3, LV_PART_MAIN);
          lv_obj_set_style_pad_top(hdr, 12, LV_PART_MAIN);
          lv_obj_set_style_pad_bottom(hdr, 4, LV_PART_MAIN);
          printed_done_header = true;
        }
        build_todo_row(body, i);
      }
    }

    // "+ neue aufgabe" button
    lv_obj_t *add = lv_obj_create(body);
    lv_obj_remove_style_all(add);
    lv_obj_set_size(add, LV_PCT(100), 56);
    lv_obj_set_style_border_color(add, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(add, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(add, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_top(add, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(add, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(add, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(add, [](lv_event_t *){ open_compose_todo(); },
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus = lv_label_create(add);
    lv_label_set_text(plus, "+ NEUE AUFGABE");
    lv_obj_set_style_text_font(plus, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(plus, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(plus, 3, LV_PART_MAIN);
  } else if (current_do_tab == DO_HABITS) {
    for (int i = 0; i < g_habits_count; i++) {
      const moki_habit_t *h = &g_habits[i];
      lv_obj_t *row = lv_obj_create(body);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_style_pad_top(row, 10, LV_PART_MAIN);
      lv_obj_set_style_pad_bottom(row, 10, LV_PART_MAIN);
      lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
      lv_obj_set_style_border_color(row, lv_color_hex(MOKI_MID), LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, on_habit_increment,    LV_EVENT_CLICKED,      (void *)(intptr_t)i);
      lv_obj_add_event_cb(row, on_habit_decrement,    LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

      lv_obj_t *txt = lv_obj_create(row);
      lv_obj_remove_style_all(txt);
      lv_obj_set_flex_grow(txt, 1);
      lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);

      lv_obj_t *name = lv_label_create(txt);
      lv_label_set_text(name, h->name);
      lv_obj_set_style_text_font(name, &moki_fraunces_regular_36, LV_PART_MAIN);
      lv_obj_set_style_text_color(name, lv_color_hex(MOKI_INK), LV_PART_MAIN);

      // Streak as plain text — JetBrains Mono ships ASCII + Umlauts but not
      // ● / ○ (U+25CF / U+25CB), so dot-strings render as missing glyphs.
      char ser[32];
      if (h->streak > 0)
        snprintf(ser, sizeof(ser), "%u TAGE IN FOLGE", (unsigned)h->streak);
      else
        strcpy(ser, "NOCH KEINE SERIE");
      lv_obj_t *s = lv_label_create(txt);
      lv_label_set_text(s, ser);
      lv_obj_set_style_text_font(s, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(s, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_pad_top(s, 4, LV_PART_MAIN);

      // Count badge — tap = open detail screen
      lv_obj_t *pill = lv_obj_create(row);
      lv_obj_remove_style_all(pill);
      lv_obj_set_size(pill, 86, 56);
      lv_obj_set_style_bg_color(pill,
        lv_color_hex(h->today_count > 0 ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(pill, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_set_style_border_width(pill, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(pill, 2, LV_PART_MAIN);
      lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(pill, on_habit_open_detail, LV_EVENT_CLICKED, (void *)(intptr_t)i);

      char cnt[8];
      snprintf(cnt, sizeof(cnt), "%u×", (unsigned)h->today_count);
      lv_obj_t *cl = lv_label_create(pill);
      lv_label_set_text(cl, cnt);
      lv_obj_set_style_text_font(cl, &moki_fraunces_regular_36, LV_PART_MAIN);
      lv_obj_set_style_text_color(cl,
        lv_color_hex(h->today_count > 0 ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
    }

    // "+ neue gewohnheit" button
    lv_obj_t *add = lv_obj_create(body);
    lv_obj_remove_style_all(add);
    lv_obj_set_size(add, LV_PCT(100), 56);
    lv_obj_set_style_border_color(add, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(add, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(add, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_top(add, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(add, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(add, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(add, [](lv_event_t *){ open_compose_habit(); },
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus = lv_label_create(add);
    lv_label_set_text(plus, "+ NEUE GEWOHNHEIT");
    lv_obj_set_style_text_font(plus, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(plus, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(plus, 3, LV_PART_MAIN);
  } else { // DO_CALENDAR
    static const char *dates[] = {"19","20","21","22","23","24","25"};
    static const char *weekdays[] = {"M","D","M","D","F","S","S"};
    int today = 2;

    // Week strip
    lv_obj_t *week = lv_obj_create(body);
    lv_obj_remove_style_all(week);
    lv_obj_set_size(week, LV_PCT(100), 60);
    lv_obj_set_flex_flow(week, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(week, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 7; i++) {
      lv_obj_t *day = lv_obj_create(week);
      lv_obj_remove_style_all(day);
      lv_obj_set_size(day, 38, 60);
      lv_obj_set_flex_flow(day, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(day, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      bool active = (i == today);
      lv_obj_set_style_bg_color(day,
        lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(day, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(day, 2, LV_PART_MAIN);

      lv_obj_t *w = lv_label_create(day);
      lv_label_set_text(w, weekdays[i]);
      lv_obj_set_style_text_font(w, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(w,
        lv_color_hex(active ? MOKI_LIGHT : MOKI_DARK), LV_PART_MAIN);

      lv_obj_t *d = lv_label_create(day);
      lv_label_set_text(d, dates[i]);
      lv_obj_set_style_text_font(d, &moki_fraunces_regular_36, LV_PART_MAIN);
      lv_obj_set_style_text_color(d,
        lv_color_hex(active ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
    }

    // Upcoming events
    lv_obj_t *hdr = lv_label_create(body);
    lv_label_set_text(hdr, "KOMMEND");
    lv_obj_set_style_text_font(hdr, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(hdr, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(hdr, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_top(hdr, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(hdr, 4, LV_PART_MAIN);

    for (int i = 0; i < g_events_count; i++) {
      const moki_event_t *ev = &g_events[i];
      lv_obj_t *row = lv_obj_create(body);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_style_pad_top(row, 8, LV_PART_MAIN);
      lv_obj_set_style_pad_bottom(row, 8, LV_PART_MAIN);
      lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
      lv_obj_set_style_border_color(row, lv_color_hex(MOKI_MID), LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);

      char dh[16];
      snprintf(dh, sizeof(dh), "%s\n%s", dates[ev->day], ev->hour);
      lv_obj_t *date = lv_label_create(row);
      lv_label_set_text(date, dh);
      lv_obj_set_style_text_font(date, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(date, lv_color_hex(MOKI_INK), LV_PART_MAIN);

      lv_obj_t *col2 = lv_obj_create(row);
      lv_obj_remove_style_all(col2);
      lv_obj_set_flex_grow(col2, 1);
      lv_obj_set_flex_flow(col2, LV_FLEX_FLOW_COLUMN);

      lv_obj_t *t = lv_label_create(col2);
      lv_label_set_text(t, ev->title);
      lv_obj_set_style_text_font(t, &moki_fraunces_regular_36, LV_PART_MAIN);
      lv_obj_set_style_text_color(t, lv_color_hex(MOKI_INK), LV_PART_MAIN);

      char place_kind[64];
      const char *vis = !strcmp(ev->kind,"public") ? "öffentlich"
                       : !strcmp(ev->kind,"friends") ? "freund_innen"
                       : "privat";
      snprintf(place_kind, sizeof(place_kind), "%s · %s", ev->place, vis);
      lv_obj_t *p = lv_label_create(col2);
      lv_label_set_text(p, place_kind);
      lv_obj_set_style_text_font(p, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(p, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_pad_top(p, 3, LV_PART_MAIN);
    }

    // "+ TERMIN" button
    lv_obj_t *add = lv_obj_create(body);
    lv_obj_remove_style_all(add);
    lv_obj_set_size(add, LV_PCT(100), 56);
    lv_obj_set_style_border_color(add, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(add, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(add, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_top(add, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(add, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(add, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(add, [](lv_event_t *){ open_compose_event(); },
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus = lv_label_create(add);
    lv_label_set_text(plus, "+ NEUER TERMIN");
    lv_obj_set_style_text_font(plus, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(plus, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(plus, 3, LV_PART_MAIN);
  }
}

static void build_do(void) {
  lv_obj_t *content = build_screen_chrome(lv_scr_act(), 1);
  build_do_content(content);
}

// ----------------------------------------------------------------------------
// READ screen — buch | feed | notizen (book tab populated, others stubbed)
// ----------------------------------------------------------------------------
static void build_read_content(lv_obj_t *parent) {
  lv_obj_t *col = lv_obj_create(parent);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_left(col, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(col, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(col, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(col, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(col, 16, LV_PART_MAIN);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *bar = make_tab_bar(col);
  make_tab_button(bar, "buch",    current_read_tab == READ_BOOK,  on_read_tab_clicked, READ_BOOK);
  make_tab_button(bar, "feed",    current_read_tab == READ_FEED,  on_read_tab_clicked, READ_FEED);
  make_tab_button(bar, "notizen", current_read_tab == READ_NOTES, on_read_tab_clicked, READ_NOTES);

  if (current_read_tab == READ_BOOK) {
    // Book list — every book gets its own row (title + source + progress %).
    // Tap on a row opens SCR_READER for fullscreen reading.
    static auto on_book_row_cb = [](lv_event_t *e) {
      int new_idx = (int)(intptr_t)lv_event_get_user_data(e);
      book_select(new_idx);
      switch_screen(SCR_READER);
    };

    auto add_book_row = [&](int idx, const char *title, const char *source_lbl) {
      // Compute progress for this specific book without disturbing g_book_page.
      // book_total_length uses g_book_active_idx so we read the page from prefs.
      int saved_page = 0;
      if (idx == -2) {
        g_prefs.begin("moki", true);
        saved_page = (int)g_prefs.getUInt("book_p_m", 0);
        g_prefs.end();
      } else if (idx == -1) {
        g_prefs.begin("moki", true);
        saved_page = (int)g_prefs.getUInt(BOOK_PAGE_KEY, 0);
        g_prefs.end();
      } else {
        char k[16]; snprintf(k, sizeof(k), "book_p_%d", idx);
        g_prefs.begin("moki", true);
        saved_page = (int)g_prefs.getUInt(k, 0);
        g_prefs.end();
      }

      lv_obj_t *row = lv_obj_create(col);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_top(row, 14, LV_PART_MAIN);
      lv_obj_set_style_pad_bottom(row, 14, LV_PART_MAIN);
      lv_obj_set_style_pad_row(row, 4, LV_PART_MAIN);
      lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
      lv_obj_set_style_border_color(row, lv_color_hex(MOKI_MID), LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, on_book_row_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)idx);

      lv_obj_t *t = lv_label_create(row);
      lv_label_set_text(t, title);
      lv_obj_set_style_text_font(t, &moki_fraunces_italic_28, LV_PART_MAIN);
      lv_obj_set_style_text_color(t, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_set_width(t, LV_PCT(100));
      lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);

      // Meta line: source · progress
      lv_obj_t *meta = lv_label_create(row);
      lv_obj_set_style_text_font(meta, &moki_jetbrains_mono_18, LV_PART_MAIN);
      lv_obj_set_style_text_color(meta, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_text_letter_space(meta, 1, LV_PART_MAIN);
      lv_obj_add_flag(meta, LV_OBJ_FLAG_EVENT_BUBBLE);

      // Compute total without switching active book: peek into the right source
      size_t total_len = 0;
      if (idx == -2) total_len = strlen_P(MOKI_MANUAL_TEXT);
      else if (idx == -1) total_len = strlen_P(MOKI_BOOK_TEXT);
      else if (idx >= 0 && idx < g_book_count) {
        const char *p = g_books[idx].path;
        File f;
        if (strncmp(p, "lfs:", 4) == 0) f = LittleFS.open(p + 4, "r");
        else if (g_sd_ready)            f = SD.open(p, "r");
        if (f) { total_len = f.size(); f.close(); }
      }
      int total_pages = (int)((total_len + BOOK_CHARS_PER_PAGE - 1) / BOOK_CHARS_PER_PAGE);
      if (total_pages < 1) total_pages = 1;
      if (saved_page >= total_pages) saved_page = total_pages - 1;
      int pct = total_pages > 1 ? (saved_page * 100) / (total_pages - 1) : 0;
      if (pct > 100) pct = 100;
      char metabuf[64];
      snprintf(metabuf, sizeof(metabuf), "%s · S. %d/%d · %d%%",
               source_lbl, saved_page + 1, total_pages, pct);
      lv_label_set_text(meta, metabuf);
    };

    add_book_row(-2, "anleitung", "moki");
    add_book_row(-1, "walden", "thoreau");
    for (int bi = 0; bi < g_book_count; bi++) {
      const char *src = "lfs";
      if (strncmp(g_books[bi].path, "lfs:", 4) != 0) src = "sd";
      add_book_row(bi, g_books[bi].title, src);
    }

    if (g_book_count == 0) {
      lv_obj_t *hint = lv_label_create(col);
      lv_label_set_text(hint,
        "Lege .txt-Dateien in /books/ auf eine SD-Karte\n"
        "oder flashe sie via `pio run -t uploadfs` in den\n"
        "internen Speicher.");
      lv_obj_set_style_text_font(hint, &moki_jetbrains_mono_18, LV_PART_MAIN);
      lv_obj_set_style_text_color(hint, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(hint, LV_PCT(100));
      lv_obj_set_style_pad_top(hint, 16, LV_PART_MAIN);
    }
  } else if (current_read_tab == READ_FEED) {
    lv_obj_t *stub = lv_label_create(col);
    lv_label_set_text(stub, "feed kommt bald.");
    lv_obj_set_style_text_font(stub, &moki_fraunces_italic_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(stub, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_align(stub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(stub, LV_PCT(100));
  } else { // READ_NOTES
    // Folder filter chips
    static char g_notes_folder_filter[24] = "";    // empty = all
    static const char *FOLDERS[] = {"", "tagebuch", "küche", "gelesen", "ideen"};
    static const char *FOLDER_LBL[] = {"alle", "tagebuch", "küche", "gelesen", "ideen"};

    lv_obj_t *fstrip = lv_obj_create(col);
    lv_obj_remove_style_all(fstrip);
    lv_obj_set_size(fstrip, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fstrip, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(fstrip, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(fstrip, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(fstrip, 8, LV_PART_MAIN);

    static auto on_folder_cb = [](lv_event_t *e) {
      const char *id = (const char *)lv_event_get_user_data(e);
      strncpy(g_notes_folder_filter, id, sizeof(g_notes_folder_filter)-1);
      g_notes_folder_filter[sizeof(g_notes_folder_filter)-1] = 0;
      switch_screen(SCR_READ);
    };
    for (int i = 0; i < 5; i++) {
      bool active = !strcmp(g_notes_folder_filter, FOLDERS[i]);
      lv_obj_t *c = lv_obj_create(fstrip);
      lv_obj_remove_style_all(c);
      lv_obj_set_size(c, LV_SIZE_CONTENT, 36);
      lv_obj_set_style_bg_color(c, lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(c, lv_color_hex(active ? MOKI_INK : MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(c, 2, LV_PART_MAIN);
      lv_obj_set_style_pad_left(c, 10, LV_PART_MAIN);
      lv_obj_set_style_pad_right(c, 10, LV_PART_MAIN);
      lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(c, on_folder_cb, LV_EVENT_CLICKED, (void *)FOLDERS[i]);
      lv_obj_t *l = lv_label_create(c);
      lv_label_set_text(l, FOLDER_LBL[i]);
      lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(l, lv_color_hex(active ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
      lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
    }

    // Pinned-first ordering with folder filter applied
    int order[MAX_NOTES]; int o = 0;
    auto matches = [&](int i) {
      if (g_notes_folder_filter[0] == 0) return true;
      return strcmp(g_notes[i].folder, g_notes_folder_filter) == 0;
    };
    for (int i = 0; i < g_notes_count; i++) if (g_notes[i].pinned && matches(i)) order[o++] = i;
    for (int i = 0; i < g_notes_count; i++) if (!g_notes[i].pinned && matches(i)) order[o++] = i;

    for (int oi = 0; oi < o; oi++) {
      int i = order[oi];
      const moki_note_t *nt = &g_notes[i];
      lv_obj_t *row = lv_obj_create(col);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_style_pad_top(row, 12, LV_PART_MAIN);
      lv_obj_set_style_pad_bottom(row, 12, LV_PART_MAIN);
      lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
      lv_obj_set_style_border_color(row, lv_color_hex(MOKI_MID), LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, on_note_open, LV_EVENT_CLICKED, (void *)(intptr_t)i);

      char title[80];
      snprintf(title, sizeof(title), "%s%s", nt->pinned ? "* " : "", nt->title);
      lv_obj_t *t = lv_label_create(row);
      lv_label_set_text(t, title);
      lv_obj_set_style_text_font(t, &moki_fraunces_regular_36, LV_PART_MAIN);
      lv_obj_set_style_text_color(t, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
      lv_obj_set_width(t, LV_PCT(100));

      char meta[64];
      snprintf(meta, sizeof(meta), "%s · %s",
               nt->folder[0] ? nt->folder : "—",
               !strcmp(nt->visibility,"public") ? "öffentlich"
               : !strcmp(nt->visibility,"friends") ? "freund_innen"
               : "privat");
      lv_obj_t *m = lv_label_create(row);
      lv_label_set_text(m, meta);
      lv_obj_set_style_text_font(m, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(m, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_text_letter_space(m, 1, LV_PART_MAIN);
      lv_obj_set_style_pad_top(m, 4, LV_PART_MAIN);
    }

    // + neue notiz
    lv_obj_t *add = lv_obj_create(col);
    lv_obj_remove_style_all(add);
    lv_obj_set_size(add, LV_PCT(100), 56);
    lv_obj_set_style_border_color(add, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(add, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(add, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(add, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(add, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(add, on_note_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus = lv_label_create(add);
    lv_label_set_text(plus, "+ NEUE NOTIZ");
    lv_obj_set_style_text_font(plus, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(plus, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(plus, 3, LV_PART_MAIN);
  }
}

static void build_read(void) {
  lv_obj_t *content = build_screen_chrome(lv_scr_act(), 2);
  build_read_content(content);
}

// ----------------------------------------------------------------------------
// MOOD picker — 8 preset chips
// ----------------------------------------------------------------------------
static void build_mood(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 16, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 14, LV_PART_MAIN);

  // Back row
  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, LV_PCT(100), 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_back_home, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← ZURÜCK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);

  lv_obj_t *kicker = lv_label_create(scr);
  lv_label_set_text(kicker, "TEILEN");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "wonach ist dir?");
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  lv_obj_t *sub = lv_label_create(scr);
  lv_label_set_text(sub,
    "wird im nächsten sync im freundeskreis sichtbar.\ngilt bis mitternacht.");
  lv_obj_set_style_text_font(sub, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(sub, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_width(sub, LV_PCT(100));
  lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(sub, 4, LV_PART_MAIN);

  // 4×2 grid of mood chips
  lv_obj_t *grid = lv_obj_create(scr);
  lv_obj_remove_style_all(grid);
  lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(grid, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_column(grid, 10, LV_PART_MAIN);

  for (int i = 0; i < MOOD_COUNT; i++) {
    const moki_mood_def_t *m = &MOOD_PRESETS[i];
    bool active = (strcmp(g_active_mood, m->id) == 0);

    lv_obj_t *chip = lv_obj_create(grid);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, LV_PCT(48), 96);
    lv_obj_set_style_bg_color(chip,
        lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip,
        lv_color_hex(active ? MOKI_INK : MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, active ? 2 : 1, LV_PART_MAIN);
    lv_obj_set_style_radius(chip, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chip, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chip, on_mood_picked, LV_EVENT_CLICKED, (void *)m->id);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, m->label);
    lv_obj_set_style_text_font(label, &moki_fraunces_regular_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(label,
        lv_color_hex(active ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(chip);
    lv_label_set_text(hint, m->hint);
    lv_obj_set_style_text_font(hint, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint,
        lv_color_hex(active ? MOKI_LIGHT : MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(hint, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_top(hint, 4, LV_PART_MAIN);
  }

  if (g_active_mood[0]) {
    lv_obj_t *clear = lv_obj_create(scr);
    lv_obj_remove_style_all(clear);
    lv_obj_set_size(clear, LV_PCT(100), 56);
    lv_obj_set_style_border_color(clear, lv_color_hex(MOKI_MID), LV_PART_MAIN);
    lv_obj_set_style_border_width(clear, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(clear, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(clear, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clear, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(clear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clear, on_mood_clear, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(clear);
    lv_label_set_text(cl, "NICHTS MEHR TEILEN");
    lv_obj_set_style_text_font(cl, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(cl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(cl, 2, LV_PART_MAIN);
  }
}

// ----------------------------------------------------------------------------
// PROFILE — own public profile preview
// ----------------------------------------------------------------------------
static void build_profile(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 16, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 16, LV_PART_MAIN);

  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, LV_PCT(100), 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_back_home, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← ZURÜCK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);

  lv_obj_t *kicker = lv_label_create(scr);
  lv_label_set_text(kicker, "DU");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, g_settings.handle);
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  lv_obj_t *handle = lv_label_create(scr);
  lv_label_set_text(handle, "HDB-3F2A · FREUND_INNEN");
  lv_obj_set_style_text_font(handle, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(handle, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(handle, 2, LV_PART_MAIN);

  lv_obj_t *bio = lv_label_create(scr);
  lv_label_set_text(bio, g_settings.bio);
  lv_obj_set_style_text_font(bio, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bio, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_width(bio, LV_PCT(100));
  lv_label_set_long_mode(bio, LV_LABEL_LONG_WRAP);

  // Stats
  lv_obj_t *stats = lv_obj_create(scr);
  lv_obj_remove_style_all(stats);
  lv_obj_set_size(stats, LV_PCT(100), 100);
  lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  static const char *kk[] = { "AUFGABEN", "GEWOHNHEITEN", "TAG" };
  char ss[3][16];
  int open = 0;
  for (int i = 0; i < g_todos_count; i++) if (!g_todos[i].done) open++;
  snprintf(ss[0], sizeof(ss[0]), "%d", open);
  snprintf(ss[1], sizeof(ss[1]), "%d", g_habits_count);
  snprintf(ss[2], sizeof(ss[2]), "14");

  for (int t = 0; t < 3; t++) {
    lv_obj_t *card = lv_obj_create(stats);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(31), 100);
    lv_obj_set_style_border_color(card, lv_color_hex(MOKI_MID), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(card);
    lv_label_set_text(k, kk[t]);
    lv_obj_set_style_text_font(k, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(k, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(k, 2, LV_PART_MAIN);

    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, ss[t]);
    lv_obj_set_style_text_font(v, &moki_fraunces_regular_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(v, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  }

  // Settings link
  lv_obj_t *settings = lv_obj_create(scr);
  lv_obj_remove_style_all(settings);
  lv_obj_set_size(settings, LV_PCT(100), 56);
  lv_obj_set_style_border_color(settings, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_border_width(settings, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(settings, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(settings, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(settings, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(settings, LV_OBJ_FLAG_CLICKABLE);
  static auto open_settings_cb = [](lv_event_t *) {
    Serial.println(F("[nav] open settings"));
    switch_screen(SCR_SETTINGS);
  };
  lv_obj_add_event_cb(settings, open_settings_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *sl = lv_label_create(settings);
  lv_label_set_text(sl, "EINSTELLUNGEN →");
  lv_obj_set_style_text_font(sl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sl, 3, LV_PART_MAIN);

#ifdef MOKI_WIFI
  // WLAN link
  lv_obj_t *wifi_link = lv_obj_create(scr);
  lv_obj_remove_style_all(wifi_link);
  lv_obj_set_size(wifi_link, LV_PCT(100), 56);
  lv_obj_set_style_border_color(wifi_link, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_border_width(wifi_link, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(wifi_link, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(wifi_link, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(wifi_link, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(wifi_link, LV_OBJ_FLAG_CLICKABLE);
  static auto open_wifi_cb = [](lv_event_t *) {
    Serial.println(F("[nav] open wifi"));
    switch_screen(SCR_WIFI);
  };
  lv_obj_add_event_cb(wifi_link, open_wifi_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *wl = lv_label_create(wifi_link);
  char wbuf[40];
  if (g_wifi.connected)      snprintf(wbuf, sizeof(wbuf), "WLAN · %s →", g_wifi.ssid);
  else if (g_wifi.enabled)   snprintf(wbuf, sizeof(wbuf), "WLAN · suche … →");
  else                       snprintf(wbuf, sizeof(wbuf), "WLAN · einrichten →");
  lv_label_set_text(wl, wbuf);
  lv_obj_set_style_text_font(wl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(wl, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(wl, 3, LV_PART_MAIN);
#endif

  // ── DISKOVERY: announce me to the mesh ───────────────────────────────
  // Manual self-advert button — useful right after setting handle/joining
  // a new room, when Lucas wants to be discovered by friends NOW (not
  // waiting for the 15-minute auto-advert).
  lv_obj_t *advert = lv_obj_create(scr);
  lv_obj_remove_style_all(advert);
  lv_obj_set_size(advert, LV_PCT(100), 56);
  lv_obj_set_style_border_color(advert, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_border_width(advert, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(advert, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(advert, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(advert, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(advert, LV_OBJ_FLAG_CLICKABLE);
  static auto on_advert_cb = [](lv_event_t *) {
    if (!g_settings.lora_tx_armed) {
      show_toast("ANTENNE FREIGEBEN");
      return;
    }
    bool ok = moki_mesh_advert(g_settings.handle);
    show_toast(ok ? "BIN IM MESH SICHTBAR" : "ADVERT FEHLGESCHLAGEN");
  };
  lv_obj_add_event_cb(advert, on_advert_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *al = lv_label_create(advert);
  lv_label_set_text(al, "MICH IM MESH ZEIGEN");
  lv_obj_set_style_text_font(al, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(al, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(al, 3, LV_PART_MAIN);
}

// ----------------------------------------------------------------------------
// HABIT DETAIL — 12-week × 7-day GitHub-style heatmap
//
// Five grey levels chosen to survive the disp_flush luminance threshold
// (y8 < 80 → 0, y8 > 175 → 255). 0 and 4 hit pure PAPER and pure INK; the
// three mid levels stay inside the unsnapped band for visible greys.
// ----------------------------------------------------------------------------
static const uint32_t HEAT_COLORS[5] = {
  0xE8E2D1u,   // 0 — PAPER (white)
  0xA5A5A5u,   // 1 — light grey, y8 ≈ 165 → gray4 = 10
  0x7A7A7Au,   // 2 — mid  grey, y8 ≈ 122 → gray4 = 7
  0x5A5A5Au,   // 3 — dark grey, y8 ≈  90 → gray4 = 5
  0x1A1612u,   // 4 — INK (black)
};

static void build_habit_detail(void) {
  if (g_active_habit < 0 || g_active_habit >= g_habits_count) {
    switch_screen(SCR_DO);
    return;
  }
  const moki_habit_t *h = &g_habits[g_active_habit];
  const int idx = g_active_habit;

  lv_obj_clean(lv_scr_act());
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 14, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 12, LV_PART_MAIN);

  // Header row — back left, delete right
  lv_obj_t *header = lv_obj_create(scr);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, LV_PCT(100), 36);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *bl = lv_label_create(header);
  lv_label_set_text(bl, "← ZURÜCK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);
  lv_obj_add_flag(bl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(bl, on_habit_back, LV_EVENT_CLICKED, NULL);

  lv_obj_t *del = lv_label_create(header);
  lv_label_set_text(del, "LÖSCHEN");
  lv_obj_set_style_text_font(del, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(del, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(del, 2, LV_PART_MAIN);
  lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(del, on_habit_delete, LV_EVENT_CLICKED, NULL);

  // Kicker + title
  lv_obj_t *kicker = lv_label_create(scr);
  lv_label_set_text(kicker, "GEWOHNHEIT");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, h->name);
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  // Month labels (rough, last cell = today)
  lv_obj_t *months = lv_obj_create(scr);
  lv_obj_remove_style_all(months);
  lv_obj_set_size(months, LV_PCT(100), 24);
  lv_obj_set_flex_flow(months, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(months, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(months, 38, LV_PART_MAIN);  // align with grid
  static const char *MONTHS[] = { "FEB", "MÄR", "APR" };
  for (int m = 0; m < 3; m++) {
    lv_obj_t *l = lv_label_create(months);
    lv_label_set_text(l, MONTHS[m]);
    lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(l, 2, LV_PART_MAIN);
  }

  // 12-week × 7-day heatmap grid (with weekday labels on the left)
  lv_obj_t *gridrow = lv_obj_create(scr);
  lv_obj_remove_style_all(gridrow);
  lv_obj_set_size(gridrow, LV_PCT(100), 230);
  lv_obj_set_flex_flow(gridrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(gridrow, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(gridrow, 6, LV_PART_MAIN);

  // Weekday column
  lv_obj_t *wcol = lv_obj_create(gridrow);
  lv_obj_remove_style_all(wcol);
  lv_obj_set_size(wcol, 32, 230);
  lv_obj_set_flex_flow(wcol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(wcol, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  static const char *WD[] = { "MO", "DI", "MI", "DO", "FR", "SA", "SO" };
  for (int d = 0; d < 7; d++) {
    lv_obj_t *l = lv_label_create(wcol);
    lv_label_set_text(l, WD[d]);
    lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
  }

  // Heatmap grid
  lv_obj_t *grid = lv_obj_create(gridrow);
  lv_obj_remove_style_all(grid);
  lv_obj_set_flex_grow(grid, 1);
  lv_obj_set_height(grid, 230);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  for (int w = 0; w < 12; w++) {
    lv_obj_t *col = lv_obj_create(grid);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 28, 230);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 3, LV_PART_MAIN);

    for (int d = 0; d < 7; d++) {
      int sample = h->history[w * 7 + d];
      if (sample > 4) sample = 4;
      lv_obj_t *cell = lv_obj_create(col);
      lv_obj_remove_style_all(cell);
      lv_obj_set_size(cell, 26, 26);
      lv_obj_set_style_bg_color(cell, lv_color_hex(HEAT_COLORS[sample]), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(cell, 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(cell, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
      lv_obj_set_style_border_width(cell, sample == 0 ? 1 : 0, LV_PART_MAIN);
    }
  }

  // Legend
  lv_obj_t *legend = lv_obj_create(scr);
  lv_obj_remove_style_all(legend);
  lv_obj_set_size(legend, LV_PCT(100), 30);
  lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(legend, 6, LV_PART_MAIN);

  lv_obj_t *less = lv_label_create(legend);
  lv_label_set_text(less, "WENIGER");
  lv_obj_set_style_text_font(less, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(less, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(less, 2, LV_PART_MAIN);

  for (int n = 0; n < 5; n++) {
    lv_obj_t *swatch = lv_obj_create(legend);
    lv_obj_remove_style_all(swatch);
    lv_obj_set_size(swatch, 18, 18);
    lv_obj_set_style_bg_color(swatch, lv_color_hex(HEAT_COLORS[n]), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(swatch, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(swatch, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
    lv_obj_set_style_border_width(swatch, n == 0 ? 1 : 0, LV_PART_MAIN);
  }

  lv_obj_t *more = lv_label_create(legend);
  lv_label_set_text(more, "MEHR");
  lv_obj_set_style_text_font(more, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(more, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(more, 2, LV_PART_MAIN);

  // -- Inc/Dec controls + counter pill --
  lv_obj_t *ctrl = lv_obj_create(scr);
  lv_obj_remove_style_all(ctrl);
  lv_obj_set_size(ctrl, LV_PCT(100), 80);
  lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(ctrl, 18, LV_PART_MAIN);

  lv_obj_t *minus = lv_obj_create(ctrl);
  lv_obj_remove_style_all(minus);
  lv_obj_set_size(minus, 80, 70);
  lv_obj_set_style_bg_color(minus, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(minus, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(minus, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(minus, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(minus, 2, LV_PART_MAIN);
  lv_obj_add_flag(minus, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(minus, on_habit_decrement, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
  lv_obj_t *ml = lv_label_create(minus);
  lv_label_set_text(ml, "−");
  lv_obj_set_style_text_font(ml, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(ml, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_center(ml);

  lv_obj_t *count = lv_obj_create(ctrl);
  lv_obj_remove_style_all(count);
  lv_obj_set_size(count, 110, 70);
  lv_obj_set_style_bg_color(count, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(count, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(count, 2, LV_PART_MAIN);
  char nb[12]; snprintf(nb, sizeof(nb), "%u×", (unsigned)h->today_count);
  lv_obj_t *cl = lv_label_create(count);
  lv_label_set_text(cl, nb);
  lv_obj_set_style_text_font(cl, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(cl, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_center(cl);

  lv_obj_t *plus = lv_obj_create(ctrl);
  lv_obj_remove_style_all(plus);
  lv_obj_set_size(plus, 80, 70);
  lv_obj_set_style_bg_color(plus, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(plus, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(plus, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(plus, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(plus, 2, LV_PART_MAIN);
  lv_obj_add_flag(plus, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(plus, on_habit_increment, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
  lv_obj_t *pl = lv_label_create(plus);
  lv_label_set_text(pl, "+");
  lv_obj_set_style_text_font(pl, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(pl, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_center(pl);

  // -- Stats: heute / gesamt / serie --
  uint32_t total = 0;
  int active_days = 0;
  for (int i = 0; i < 84; i++) { total += h->history[i]; if (h->history[i] > 0) active_days++; }

  lv_obj_t *stats = lv_obj_create(scr);
  lv_obj_remove_style_all(stats);
  lv_obj_set_size(stats, LV_PCT(100), 100);
  lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  static const char *kk[] = { "HEUTE", "GESAMT", "SERIE" };
  char ss[3][24];
  snprintf(ss[0], sizeof(ss[0]), "%u×",        (unsigned)h->today_count);
  snprintf(ss[1], sizeof(ss[1]), "%u",         (unsigned)total);
  snprintf(ss[2], sizeof(ss[2]), "%u",         (unsigned)h->streak);
  static const char *bb[] = { "gemacht", "in 12 wo.", "tage am stück" };

  for (int t = 0; t < 3; t++) {
    lv_obj_t *card = lv_obj_create(stats);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(31), 100);
    lv_obj_set_style_bg_color(card, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(MOKI_MID), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(card);
    lv_label_set_text(k, kk[t]);
    lv_obj_set_style_text_font(k, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(k, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(k, 2, LV_PART_MAIN);

    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, ss[t]);
    lv_obj_set_style_text_font(v, &moki_fraunces_regular_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(v, lv_color_hex(MOKI_INK), LV_PART_MAIN);

    lv_obj_t *b = lv_label_create(card);
    lv_label_set_text(b, bb[t]);
    lv_obj_set_style_text_font(b, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(b, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(b, 1, LV_PART_MAIN);
  }
}

// ----------------------------------------------------------------------------
// CHATS — list of conversations (direct/group/public + reset cadences)
// ----------------------------------------------------------------------------
static void build_chat_row(lv_obj_t *parent, const moki_chat_t *c) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_top(row, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(row, 14, LV_PART_MAIN);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(row, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  // Identify by index — find c in SAMPLE_CHATS
  int idx = (int)(c - SAMPLE_CHATS);
  lv_obj_add_event_cb(row, on_chat_open, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

  // Kind glyph
  lv_obj_t *g = lv_label_create(row);
  lv_label_set_text(g, chat_kind_glyph(c->kind));
  lv_obj_set_style_text_font(g, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(g, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  // Center column — name + last preview + (reset hint)
  lv_obj_t *col = lv_obj_create(row);
  lv_obj_remove_style_all(col);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *name = lv_label_create(col);
  lv_label_set_text(name, c->name);
  lv_obj_set_style_text_font(name, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(name, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  lv_obj_t *last = lv_label_create(col);
  lv_label_set_text(last, c->last);
  lv_obj_set_style_text_font(last, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(last, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_label_set_long_mode(last, LV_LABEL_LONG_DOT);
  lv_obj_set_width(last, LV_PCT(100));
  lv_obj_set_style_pad_top(last, 4, LV_PART_MAIN);

  if (c->reset && c->reset[0]) {
    lv_obj_t *r = lv_label_create(col);
    lv_label_set_text(r, chat_reset_phrase(c->reset));
    lv_obj_set_style_text_font(r, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(r, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_top(r, 4, LV_PART_MAIN);
  }

  // Right column — timestamp + unread badge
  lv_obj_t *right = lv_obj_create(row);
  lv_obj_remove_style_all(right);
  lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

  lv_obj_t *ts = lv_label_create(right);
  lv_label_set_text(ts, c->ts);
  lv_obj_set_style_text_font(ts, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(ts, lv_color_hex(MOKI_DARK), LV_PART_MAIN);

  if (c->unread > 0) {
    lv_obj_t *badge = lv_obj_create(right);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 36, 36);
    lv_obj_set_style_bg_color(badge, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_top(badge, 6, LV_PART_MAIN);

    char num[8]; snprintf(num, sizeof(num), "%u", (unsigned)c->unread);
    lv_obj_t *n = lv_label_create(badge);
    lv_label_set_text(n, num);
    lv_obj_set_style_text_font(n, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(n, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_center(n);
  }
}

// Helper: walk the ring buffer to find the latest message + last ts_ms for a
// given filter (channel_idx). Returns pointer to msg or NULL.
static const moki_lora_msg_t *find_latest_for_channel(int channel_idx) {
  const moki_lora_msg_t *latest = NULL;
  for (int n = 0; n < g_lora_msg_count; n++) {
    const moki_lora_msg_t *m = &g_lora_msgs[n];
    if (m->channel_idx != channel_idx) continue;
    if (!latest || m->ts_ms > latest->ts_ms) latest = m;
  }
  return latest;
}

// Helper for DM: find latest message from a contact (matched by name prefix).
static const moki_lora_msg_t *find_latest_from_contact(const char *contact_name) {
  const moki_lora_msg_t *latest = NULL;
  for (int n = 0; n < g_lora_msg_count; n++) {
    const moki_lora_msg_t *m = &g_lora_msgs[n];
    if (m->channel_idx != -1) continue;   // only DMs
    if (strncmp(m->from, contact_name, sizeof(m->from)) != 0) continue;
    if (!latest || m->ts_ms > latest->ts_ms) latest = m;
  }
  return latest;
}

// Format a relative-time hint ("vor 5 min", "jetzt", "älter").
static void format_age(uint32_t msg_ts_ms, char *out, size_t out_size) {
  uint32_t now = millis();
  if (msg_ts_ms == 0 || msg_ts_ms > now) {
    snprintf(out, out_size, "älter");
    return;
  }
  uint32_t age_s = (now - msg_ts_ms) / 1000;
  if      (age_s < 60)     snprintf(out, out_size, "vor %lus", (unsigned long)age_s);
  else if (age_s < 3600)   snprintf(out, out_size, "vor %lum", (unsigned long)(age_s / 60));
  else if (age_s < 86400)  snprintf(out, out_size, "vor %luh", (unsigned long)(age_s / 3600));
  else                     snprintf(out, out_size, "älter");
}

// Format absolute time (HH:MM) using session-uptime relative to RTC if known.
static void format_clock(uint32_t msg_ts_ms, char *out, size_t out_size) {
  if (!g_rtc_ready) {
    format_age(msg_ts_ms, out, out_size);
    return;
  }
  // Reconstruct: msg_abs_seconds = (rtc_now_seconds) - (millis_now - msg_ts_ms)/1000
  uint32_t now = millis();
  if (msg_ts_ms == 0 || msg_ts_ms > now) {
    snprintf(out, out_size, "älter");
    return;
  }
  RTC_DateTime rtc = g_rtc.getDateTime();
  int32_t age_s = (int32_t)((now - msg_ts_ms) / 1000);
  // Subtract age from current rtc HH:MM:SS → message time. Naive (no day-rollover).
  int32_t total_s = rtc.hour * 3600 + rtc.minute * 60 + rtc.second - age_s;
  while (total_s < 0) total_s += 86400;
  int32_t hh = (total_s / 3600) % 24;
  int32_t mm = (total_s / 60) % 60;
  snprintf(out, out_size, "%02ld:%02ld", (long)hh, (long)mm);
}

// One row per chat. Tap → set chat_target + jump into chat-detail.
typedef struct {
  const char *title;
  const char *subtitle;     // last-msg preview or empty
  const char *meta;         // RHS info (count or time, or empty)
  int kind;                 // 0 = channel, 1 = dm, 2 = special (foreign)
  int idx;                  // channel index or contact index
} moki_chat_row_t;

static void build_chat_row_dynamic(lv_obj_t *parent, const moki_chat_row_t *r) {
  static auto on_row_tap = [](lv_event_t *e) {
    intptr_t packed = (intptr_t)lv_event_get_user_data(e);
    g_chat_target_kind = (int)(packed >> 16);
    g_chat_target_idx  = (int)(packed & 0xFFFF);
    g_active_chat = -1;   // disable legacy mockup-list path
    switch_screen(SCR_CHAT_DETAIL);
  };
  intptr_t packed = ((intptr_t)r->kind << 16) | (intptr_t)r->idx;

  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_top(row, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(row, 14, LV_PART_MAIN);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(row, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, on_row_tap, LV_EVENT_CLICKED, (void *)packed);

  // Left column: title + subtitle stacked
  lv_obj_t *left = lv_obj_create(row);
  lv_obj_remove_style_all(left);
  lv_obj_set_flex_grow(left, 1);
  lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
  // LVGL doesn't bubble click events by default. The `left` container fills
  // most of the row's tappable area, so without EVENT_BUBBLE the row never
  // gets the click.
  lv_obj_clear_flag(left, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(left, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t *t = lv_label_create(left);
  lv_label_set_text(t, r->title);
  lv_obj_set_style_text_font(t, &moki_fraunces_italic_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);

  if (r->subtitle && r->subtitle[0]) {
    lv_obj_t *s = lv_label_create(left);
    lv_label_set_text(s, r->subtitle);
    lv_obj_set_style_text_font(s, &moki_jetbrains_mono_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(s, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_width(s, LV_PCT(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_top(s, 4, LV_PART_MAIN);
    lv_obj_add_flag(s, LV_OBJ_FLAG_EVENT_BUBBLE);
  }

  // Right meta: time or count
  if (r->meta && r->meta[0]) {
    lv_obj_t *m = lv_label_create(row);
    lv_label_set_text(m, r->meta);
    lv_obj_set_style_text_font(m, &moki_jetbrains_mono_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(m, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_pad_left(m, 12, LV_PART_MAIN);
    lv_obj_add_flag(m, LV_OBJ_FLAG_EVENT_BUBBLE);
  }
}

static void build_chats_content(lv_obj_t *parent) {
  lv_obj_t *col = lv_obj_create(parent);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_left(col, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(col, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(col, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(col, 12, LV_PART_MAIN);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *kicker = lv_label_create(col);
  lv_label_set_text(kicker, "GESPRÄCHE");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  lv_obj_t *title_lbl = lv_label_create(col);
  lv_label_set_text(title_lbl, "ein kleiner kreis.");
  lv_obj_set_style_text_font(title_lbl, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title_lbl, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(title_lbl, 8, LV_PART_MAIN);

  // ── ROOMS — one row per MeshCore channel ─────────────────────────────
  for (int i = 0; i < g_num_channels; i++) {
    char title_buf[32]; snprintf(title_buf, sizeof(title_buf), "#%s", g_channels[i].name);
    char sub_buf[80] = "—";
    char meta_buf[16] = "";
    const moki_lora_msg_t *latest = find_latest_for_channel(i);
    if (latest) {
      // Build "<sender>: <body>" preview
      char preview[120];
      char safe_text[64]; size_t tn = latest->len;
      if (tn > sizeof(safe_text) - 1) tn = sizeof(safe_text) - 1;
      for (size_t k = 0; k < tn; k++) {
        uint8_t b = (uint8_t)latest->text[k];
        safe_text[k] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
      }
      safe_text[tn] = 0;
      snprintf(preview, sizeof(preview), "%s: %s", latest->from, safe_text);
      strncpy(sub_buf, preview, sizeof(sub_buf) - 1);
      sub_buf[sizeof(sub_buf) - 1] = 0;
      format_clock(latest->ts_ms, meta_buf, sizeof(meta_buf));
    }
    // No strdup — build_chat_row_dynamic copies via lv_label_set_text.
    moki_chat_row_t r = { title_buf, sub_buf, meta_buf, /*kind=*/0, /*idx=*/i };
    build_chat_row_dynamic(col, &r);
  }

  // ── DM CONTACTS — one row per discovered Moki ────────────────────────
  int n_contacts = moki_mesh_contact_count();
  if (n_contacts > 0) {
    lv_obj_t *gap = lv_obj_create(col);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, LV_PCT(100), 16);

    lv_obj_t *k2 = lv_label_create(col);
    lv_label_set_text(k2, "DEINE MOKIS");
    lv_obj_set_style_text_font(k2, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(k2, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(k2, 3, LV_PART_MAIN);

    for (int i = 0; i < n_contacts; i++) {
      char name[40]; uint8_t key4[4];
      if (!moki_mesh_get_contact(i, name, sizeof(name), key4)) continue;
      char sub_buf[80]; char meta_buf[16] = "";
      const moki_lora_msg_t *latest = find_latest_from_contact(name);
      if (latest) {
        char safe_text[64]; size_t tn = latest->len;
        if (tn > sizeof(safe_text) - 1) tn = sizeof(safe_text) - 1;
        for (size_t k = 0; k < tn; k++) {
          uint8_t b = (uint8_t)latest->text[k];
          safe_text[k] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        safe_text[tn] = 0;
        strncpy(sub_buf, safe_text, sizeof(sub_buf) - 1);
        sub_buf[sizeof(sub_buf) - 1] = 0;
        format_clock(latest->ts_ms, meta_buf, sizeof(meta_buf));
      } else {
        snprintf(sub_buf, sizeof(sub_buf), "id %02x%02x%02x%02x — noch keine nachricht",
                 key4[0], key4[1], key4[2], key4[3]);
      }
      moki_chat_row_t r = { name, sub_buf, meta_buf, /*kind=*/1, /*idx=*/i };
      build_chat_row_dynamic(col, &r);
    }
  }
}

static void build_chats(void) {
  lv_obj_t *content = build_screen_chrome(lv_scr_act(), 3);
  build_chats_content(content);
}

// ----------------------------------------------------------------------------
// MAP — stylized cartography + "in der nähe" peer list
// ----------------------------------------------------------------------------
// Map-Rendering: KEIN lv_canvas. Stattdessen ein leeres lv_obj mit Custom
// LV_EVENT_DRAW_MAIN Handler — wir zeichnen direkt mit lv_draw_line in den
// LVGL-Display-Buffer. Spart PSRAM (kein Canvas-Buffer), funktioniert robust
// auf E-Ink. INDEXED_2BIT-Canvas hatte zwar die Pixel im Speicher, aber die
// LVGL-Display-Conversion zu unserem 32-bit Buffer wirft das stumm weg.
static lv_point_t  *g_map_proj_pts = nullptr;   // [2048] PSRAM-heap (8KB)
static lv_obj_t   *g_map_obj = nullptr;     // map container, for redraw hooks
// Pan-Drag-Tracking
static int  g_drag_start_x = 0, g_drag_start_y = 0;
static float g_drag_start_lat = 0.0f, g_drag_start_lon = 0.0f;
static bool  g_drag_active = false;
static uint32_t g_press_start_ms = 0;       // für Long-Press-Erkennung

// ── Self-Position (M6.13) ───────────────────────────────────────────────
// Ich-Marker auf Karte — auch ohne aktiven GPS-Fix. Wir merken uns:
//   - source=0: keine Position bekannt → kein Marker
//   - source=1: aktueller GPS-Fix (frisch)
//   - source=2: letzter Fix (älter) — wird aus NVS geladen
//   - source=3: manuell gesetzt via "ich bin hier"-Button
typedef struct {
  float    lat;
  float    lon;
  uint8_t  source;        // 0/1/2/3
  uint32_t set_ts_ms;     // millis() beim Setzen
} moki_self_pos_t;
static moki_self_pos_t g_self_pos = { 0.0f, 0.0f, 0, 0 };

static void self_pos_save(void) {
  g_prefs.begin("moki", false);
  g_prefs.putFloat("self_lat", g_self_pos.lat);
  g_prefs.putFloat("self_lon", g_self_pos.lon);
  g_prefs.putUChar("self_src", g_self_pos.source);
  g_prefs.end();
}

static void self_pos_load(void) {
  g_prefs.begin("moki", true);
  g_self_pos.lat    = g_prefs.getFloat("self_lat", 0.0f);
  g_self_pos.lon    = g_prefs.getFloat("self_lon", 0.0f);
  uint8_t src       = g_prefs.getUChar("self_src", 0);
  g_prefs.end();
  // Persistierte Source ist immer "stale" beim Boot → degrade fresh→last
  if (src == 1) src = 2;
  g_self_pos.source = src;
  g_self_pos.set_ts_ms = millis();
  Serial.printf("[self] loaded source=%u lat=%.4f lon=%.4f\n",
                (unsigned)g_self_pos.source, g_self_pos.lat, g_self_pos.lon);
}

static void self_pos_update_from_gps(void) {
  // Wird bei jedem frischen GPS-Fix (1Hz) aufgerufen.
  static uint32_t last_persist = 0;
  static uint32_t last_redraw_ms = 0;
  static float    last_redraw_lat = 0.0f;
  static float    last_redraw_lon = 0.0f;

  uint8_t prev_source = g_self_pos.source;
  float   prev_lat    = g_self_pos.lat;
  float   prev_lon    = g_self_pos.lon;

  g_self_pos.lat       = g_gps.lat_deg;
  g_self_pos.lon       = g_gps.lon_deg;
  g_self_pos.source    = 1;   // fresh GPS
  g_self_pos.set_ts_ms = millis();

  // Persist max alle 60s — schont Flash-Wear
  if (millis() - last_persist > 60000) {
    self_pos_save();
    last_persist = millis();
  }

  // Map-Invalidate wenn:
  //   - first-fix (prev_source != 1, also Übergang von 0/2/3 → 1) oder
  //   - Position um mehr als ~50m bewegt (0.0005°) oder
  //   - >30s seit letztem Redraw
  // Bei E-Ink keinesfalls bei jedem 1Hz-Fix neuzeichnen — flackert sonst.
  bool first_fix    = (prev_source != 1);
  float dlat        = fabsf(g_self_pos.lat - last_redraw_lat);
  float dlon        = fabsf(g_self_pos.lon - last_redraw_lon);
  bool moved        = (dlat > 0.0005f || dlon > 0.0005f);
  bool stale_redraw = (millis() - last_redraw_ms) > 30000;

  if ((first_fix || moved || stale_redraw) &&
      g_map_obj && current_screen == SCR_MAP) {
    lv_obj_invalidate(g_map_obj);
    last_redraw_ms  = millis();
    last_redraw_lat = g_self_pos.lat;
    last_redraw_lon = g_self_pos.lon;
    if (first_fix) {
      Serial.printf("[self] FIRST FIX → redraw map · lat=%.4f lon=%.4f\n",
                    g_self_pos.lat, g_self_pos.lon);
    }
  }
  (void)prev_lat; (void)prev_lon;
}

static void self_pos_set_manual(float lat, float lon) {
  g_self_pos.lat       = lat;
  g_self_pos.lon       = lon;
  g_self_pos.source    = 3;   // manual
  g_self_pos.set_ts_ms = millis();
  self_pos_save();
}

// ── Pins (M6.8) ─────────────────────────────────────────────────────────
// Eigene Markierungen auf der Karte. Persistiert auf LittleFS /pins.bin.
// Format: header (magic "PIN2" + count) + N × pin record.
#define MOKI_MAX_PINS 32
typedef struct {
  float    lat;
  float    lon;
  char     name[40];
  char     note[120];      // Notiz/Beschreibung (optional)
  uint32_t created_ts;
} moki_pin_t;
static moki_pin_t *g_pins = nullptr;       // [MOKI_MAX_PINS] PSRAM-heap (5.5KB)
static uint8_t    g_pins_n = 0;
static int        g_active_pin = -1;   // Index des in PIN_DETAIL geöffneten Pins

static void pins_save(void) {
  if (!g_fs_ready) return;
  File f = LittleFS.open("/pins.bin", "w");
  if (!f) return;
  f.write((const uint8_t *)"PIN2", 4);
  f.write(&g_pins_n, 1);
  for (uint8_t i = 0; i < g_pins_n; i++) {
    f.write((const uint8_t *)&g_pins[i], sizeof(moki_pin_t));
  }
  f.close();
  Serial.printf("[pins] saved %u\n", (unsigned)g_pins_n);
}

static void pins_load(void) {
  g_pins_n = 0;
  if (!g_fs_ready) return;
  File f = LittleFS.open("/pins.bin", "r");
  if (!f) return;
  char magic[4];
  if (f.read((uint8_t *)magic, 4) != 4) { f.close(); return; }
  if (memcmp(magic, "PIN2", 4) != 0) {
    // Alte v1-Datei oder unbekannt → ignorieren (frische Liste)
    Serial.printf("[pins] schema mismatch, starting fresh\n");
    f.close(); return;
  }
  uint8_t n;
  if (f.read(&n, 1) != 1 || n > MOKI_MAX_PINS) { f.close(); return; }
  for (uint8_t i = 0; i < n; i++) {
    if (f.read((uint8_t *)&g_pins[i], sizeof(moki_pin_t)) != sizeof(moki_pin_t)) {
      f.close(); return;
    }
  }
  g_pins_n = n;
  f.close();
  Serial.printf("[pins] loaded %u\n", (unsigned)g_pins_n);
}

static bool pins_add(float lat, float lon, const char *name) {
  if (g_pins_n >= MOKI_MAX_PINS) return false;
  g_pins[g_pins_n].lat = lat;
  g_pins[g_pins_n].lon = lon;
  strncpy(g_pins[g_pins_n].name, name ? name : "ort",
          sizeof(g_pins[g_pins_n].name) - 1);
  g_pins[g_pins_n].name[sizeof(g_pins[g_pins_n].name) - 1] = 0;
  g_pins[g_pins_n].note[0] = 0;
  g_pins[g_pins_n].created_ts = millis();
  g_pins_n++;
  pins_save();
  return true;
}

static bool pins_delete(int idx) {
  if (idx < 0 || idx >= (int)g_pins_n) return false;
  for (int i = idx; i < (int)g_pins_n - 1; i++) g_pins[i] = g_pins[i + 1];
  g_pins_n--;
  pins_save();
  return true;
}

static bool pins_rename(int idx, const char *new_name) {
  if (idx < 0 || idx >= (int)g_pins_n || !new_name) return false;
  strncpy(g_pins[idx].name, new_name, sizeof(g_pins[idx].name) - 1);
  g_pins[idx].name[sizeof(g_pins[idx].name) - 1] = 0;
  pins_save();
  return true;
}

static bool pins_set_note(int idx, const char *note) {
  if (idx < 0 || idx >= (int)g_pins_n) return false;
  if (!note) note = "";
  strncpy(g_pins[idx].note, note, sizeof(g_pins[idx].note) - 1);
  g_pins[idx].note[sizeof(g_pins[idx].note) - 1] = 0;
  pins_save();
  return true;
}

// M6.10 — Pin-Sharing via Mesh
// Format der DM-Payload: MOKI_PLACE:<lat>:<lon>:<name>|<note>
// (note optional, alles nach erstem '|' wenn vorhanden)
static String pin_share_format(const moki_pin_t *p) {
  String out = "MOKI_PLACE:";
  out += String(p->lat, 6); out += ":";
  out += String(p->lon, 6); out += ":";
  out += p->name;
  if (p->note[0]) { out += "|"; out += p->note; }
  return out;
}

// Wird von moki_mesh.cpp aufgerufen, wenn ein DM mit MOKI_PLACE-Prefix
// reinkommt. Parsen und automatisch in g_pins[] anhängen.
extern "C" void moki_pin_share_received(const char *from, const char *payload);
extern "C" void moki_pin_share_received(const char *from, const char *payload) {
  if (!payload || !*payload) return;
  // payload = "<lat>:<lon>:<name>|<note>"
  String p = payload;
  int c1 = p.indexOf(':');
  int c2 = p.indexOf(':', c1 + 1);
  if (c1 < 0 || c2 < 0) return;
  float lat = p.substring(0, c1).toFloat();
  float lon = p.substring(c1 + 1, c2).toFloat();
  String rest = p.substring(c2 + 1);
  int pipe = rest.indexOf('|');
  String name, note;
  if (pipe >= 0) { name = rest.substring(0, pipe); note = rest.substring(pipe + 1); }
  else            name = rest;

  // Anhängen mit Quelle im Namen — leichter Anti-Verwechselungs-Hint
  char display_name[40];
  snprintf(display_name, sizeof(display_name), "%s · %s",
           name.c_str(), from ? from : "?");

  if (g_pins_n >= MOKI_MAX_PINS) {
    Serial.printf("[pin] inbox full — discard from %s '%s'\n", from, name.c_str());
    return;
  }
  if (pins_add(lat, lon, display_name)) {
    pins_set_note(g_pins_n - 1, note.c_str());
    Serial.printf("[pin] received from %s: '%s' @ %.4f,%.4f\n",
                  from, name.c_str(), lat, lon);
    if (g_map_obj && current_screen == SCR_MAP) lv_obj_invalidate(g_map_obj);
  }
}

// Custom-Draw-Handler: zeichnet die Karte direkt in den LVGL-Display-Buffer,
// wenn der Map-Container neu gerendert werden muss.
static void map_draw_event_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);

  int box_x = coords.x1;
  int box_y = coords.y1;
  int box_w = coords.x2 - coords.x1 + 1;
  int box_h = coords.y2 - coords.y1 + 1;
  int cx_screen = box_x + box_w / 2;
  int cy_screen = box_y + box_h / 2;

  // Hintergrund (Papier) füllen
  lv_draw_rect_dsc_t bg;
  lv_draw_rect_dsc_init(&bg);
  bg.bg_color = lv_color_hex(MOKI_PAPER);
  bg.bg_opa = LV_OPA_COVER;
  lv_draw_rect(draw_ctx, &bg, &coords);

  if (!g_map.ready) return;

  viewport_compute_scale(&g_viewport, box_w, box_h);

  uint32_t t_start = millis();
  uint32_t lines_drawn = 0, lines_skipped = 0;

  // ── LINES ───────────────────────────────────────────────────────────────
  size_t off = g_map.off_lines;
  for (uint32_t i = 0; i < g_map.n_lines; i++) {
    if (off + 4 > g_map.buf_len) break;
    uint8_t  type_id = g_map.buf[off];
    uint8_t  zoom_min = g_map.buf[off + 1];
    uint16_t n_pts;
    memcpy(&n_pts, g_map.buf + off + 2, 2);
    size_t   data_off = off + 4;
    size_t   line_size = 4 + (size_t)n_pts * 4;

    if (zoom_min > g_viewport.zoom_idx) { off += line_size; continue; }

    // Line-Type-Filter pro Zoom: bei niedrigerem Zoom weniger Strichgewirr,
    // Stadt-Detail erst bei CITY-Zoom.
    //   COUNTRY: nur MOTORWAY + RIVER → Übersicht über Süddeutschland
    //   REGION:  + PRIMARY → Stadt-Stadt-Verbindungen sichtbar
    //   CITY:    alles inkl. RAIL
    if (g_viewport.zoom_idx == MAP_Z_COUNTRY) {
      if (type_id != MAP_T_MOTORWAY && type_id != MAP_T_RIVER) {
        off += line_size; continue;
      }
    } else if (g_viewport.zoom_idx == MAP_Z_REGION) {
      // Schiene weg — verdoppelt sonst die Strich-Dichte
      if (type_id == MAP_T_RAIL) {
        off += line_size; continue;
      }
    }

    // Project + bbox cull (in screen-koordinaten, also bezogen auf cx/cy_screen)
    int min_sx = INT_MAX, max_sx = INT_MIN;
    int min_sy = INT_MAX, max_sy = INT_MIN;
    int n_proj = 0;
    uint16_t step = 1;
    if (n_pts > 2048) step = (uint16_t)((n_pts + 2047) / 2048);
    for (uint16_t j = 0; j < n_pts && n_proj < 2048; j += step) {
      uint16_t lat_q, lon_q;
      memcpy(&lat_q, g_map.buf + data_off + (size_t)j * 4 + 0, 2);
      memcpy(&lon_q, g_map.buf + data_off + (size_t)j * 4 + 2, 2);
      float lat = map_q_to_lat(lat_q);
      float lon = map_q_to_lon(lon_q);
      int sx, sy;
      map_project(&g_viewport, lat, lon, cx_screen, cy_screen, &sx, &sy);
      g_map_proj_pts[n_proj].x = (lv_coord_t)sx;
      g_map_proj_pts[n_proj].y = (lv_coord_t)sy;
      if (sx < min_sx) min_sx = sx;
      if (sx > max_sx) max_sx = sx;
      if (sy < min_sy) min_sy = sy;
      if (sy > max_sy) max_sy = sy;
      n_proj++;
    }

    if (max_sx < box_x || min_sx >= box_x + box_w ||
        max_sy < box_y || min_sy >= box_y + box_h ||
        n_proj < 2) {
      lines_skipped++;
      off += line_size;
      continue;
    }

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.opa = LV_OPA_COVER;
    dsc.color = lv_color_hex(MOKI_INK);
    switch (type_id) {
      case MAP_T_RIVER:
        dsc.color = lv_color_hex(MOKI_DARK);
        dsc.width = (g_viewport.zoom_idx == MAP_Z_CITY) ? 4 : 3;
        break;
      case MAP_T_MOTORWAY: dsc.width = 2; break;
      case MAP_T_PRIMARY:  dsc.color = lv_color_hex(MOKI_DARK); dsc.width = 1; break;
      case MAP_T_RAIL:     dsc.color = lv_color_hex(MOKI_DARK); dsc.width = 1; break;
      default:             dsc.width = 1; break;
    }

    // Als Polylinie: pro Segment ein lv_draw_line-Aufruf (LVGL 8.3 hat keinen
    // direkten polyline draw_ctx-Call ohne canvas).
    for (int j = 0; j + 1 < n_proj; j++) {
      lv_draw_line(draw_ctx, &dsc, &g_map_proj_pts[j], &g_map_proj_pts[j + 1]);
    }
    lines_drawn++;
    off += line_size;
  }

  // ── CITIES (priority-sorted, collision-avoiding) ───────────────────────
  // Iteriere Städte sortiert nach Bevölkerung (pt_idx). Track gezeichneter
  // (sx, sy) — wenn neue Stadt zu nah an einer schon gezeichneten ist,
  // skippen wir komplett. Größere gewinnt.
  static lv_point_t drawn_cities[64];   // bis zu 64 sichtbare Cities
  int n_drawn = 0;

  // Mindest-Pixel-Abstand pro Zoom: nicht zu eng, aber bei REGION jetzt
  // großzügiger weil weniger Striche → Karte verträgt mehr Städte.
  int min_dist_px = 60;
  if (g_viewport.zoom_idx == MAP_Z_COUNTRY) min_dist_px = 130;
  else if (g_viewport.zoom_idx == MAP_Z_REGION) min_dist_px = 95;

  uint8_t min_pop = 0;
  if (g_viewport.zoom_idx == MAP_Z_COUNTRY)      min_pop = 5;
  else if (g_viewport.zoom_idx == MAP_Z_REGION)  min_pop = 4;
  // Bei COUNTRY weiterhin city-only; bei REGION dürfen größere place=town
  // (>50K, pop_log≥4 ist 10K+ — also 50K erst >=4.7) wieder dazu.
  bool city_only = (g_viewport.zoom_idx == MAP_Z_COUNTRY);

  uint32_t cities_drawn = 0;
  uint32_t n_idx = g_map.pt_idx ? g_map.pt_idx_n : 0;
  for (uint32_t i = 0; i < n_idx; i++) {
    size_t poff = g_map.pt_idx[i].off;
    if (poff + 8 > g_map.buf_len) continue;
    uint8_t  type_id  = g_map.buf[poff];
    uint8_t  zoom_min = g_map.buf[poff + 1];
    uint16_t lat_q, lon_q;
    memcpy(&lat_q, g_map.buf + poff + 2, 2);
    memcpy(&lon_q, g_map.buf + poff + 4, 2);
    uint8_t  pop_log  = g_map.buf[poff + 6];
    uint8_t  name_len = g_map.buf[poff + 7];
    const char *name  = (const char *)(g_map.buf + poff + 8);

    if (zoom_min > g_viewport.zoom_idx) continue;
    if (pop_log < min_pop) continue;
    if (city_only && type_id != MAP_P_CITY) continue;

    float lat = map_q_to_lat(lat_q);
    float lon = map_q_to_lon(lon_q);
    int sx, sy;
    map_project(&g_viewport, lat, lon, cx_screen, cy_screen, &sx, &sy);
    if (sx < box_x || sx >= box_x + box_w || sy < box_y || sy >= box_y + box_h) continue;

    // Kollisions-Check gegen schon gezeichnete (Manhattan-Distanz, schnell)
    bool collision = false;
    for (int k = 0; k < n_drawn; k++) {
      int adx = abs(sx - drawn_cities[k].x);
      int ady = abs(sy - drawn_cities[k].y);
      if (adx < min_dist_px && ady < min_dist_px / 2) {
        collision = true;
        break;
      }
    }
    if (collision) continue;

    int dot_sz = (type_id == MAP_P_CITY) ? 6 : 4;
    if (pop_log >= 6) dot_sz = 9;
    else if (pop_log >= 5) dot_sz = 7;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(MOKI_INK);
    rd.bg_opa = LV_OPA_COVER;
    lv_area_t dot = { (lv_coord_t)(sx - dot_sz/2), (lv_coord_t)(sy - dot_sz/2),
                       (lv_coord_t)(sx - dot_sz/2 + dot_sz - 1),
                       (lv_coord_t)(sy - dot_sz/2 + dot_sz - 1) };
    lv_draw_rect(draw_ctx, &rd, &dot);

    if (name_len > 0 && name_len < 40) {
      char nbuf[40];
      memcpy(nbuf, name, name_len);
      nbuf[name_len] = 0;
      lv_draw_label_dsc_t ld;
      lv_draw_label_dsc_init(&ld);
      ld.color = lv_color_hex(MOKI_INK);
      ld.font = (pop_log >= 5) ? &moki_fraunces_italic_22 : &moki_fraunces_italic_18;
      lv_area_t la = { (lv_coord_t)(sx + dot_sz/2 + 3), (lv_coord_t)(sy - 12),
                       (lv_coord_t)(sx + dot_sz/2 + 3 + 220), (lv_coord_t)(sy + 12) };
      lv_draw_label(draw_ctx, &ld, &la, nbuf, NULL);
    }

    if (n_drawn < (int)(sizeof(drawn_cities) / sizeof(drawn_cities[0]))) {
      drawn_cities[n_drawn].x = (lv_coord_t)sx;
      drawn_cities[n_drawn].y = (lv_coord_t)sy;
      n_drawn++;
    }
    cities_drawn++;
  }

  // ── PINS (eigene Markierungen) ──────────────────────────────────────────
  for (uint8_t pi = 0; pi < g_pins_n; pi++) {
    int sx, sy;
    map_project(&g_viewport, g_pins[pi].lat, g_pins[pi].lon,
                cx_screen, cy_screen, &sx, &sy);
    if (sx < box_x || sx >= box_x + box_w || sy < box_y || sy >= box_y + box_h) continue;

    // Sterniges Quadrat (Outline) — visually distinct from city dots
    lv_draw_rect_dsc_t pin_d;
    lv_draw_rect_dsc_init(&pin_d);
    pin_d.bg_color = lv_color_hex(MOKI_PAPER);
    pin_d.bg_opa = LV_OPA_COVER;
    pin_d.border_color = lv_color_hex(MOKI_INK);
    pin_d.border_width = 2;
    pin_d.radius = 0;
    lv_area_t pa = { (lv_coord_t)(sx-7), (lv_coord_t)(sy-7),
                     (lv_coord_t)(sx+6), (lv_coord_t)(sy+6) };
    lv_draw_rect(draw_ctx, &pin_d, &pa);
    // Innenpunkt
    lv_draw_rect_dsc_t pin_inner;
    lv_draw_rect_dsc_init(&pin_inner);
    pin_inner.bg_color = lv_color_hex(MOKI_INK);
    pin_inner.bg_opa = LV_OPA_COVER;
    lv_area_t ia = { (lv_coord_t)(sx-2), (lv_coord_t)(sy-2),
                     (lv_coord_t)(sx+1), (lv_coord_t)(sy+1) };
    lv_draw_rect(draw_ctx, &pin_inner, &ia);

    // Label
    lv_draw_label_dsc_t pl;
    lv_draw_label_dsc_init(&pl);
    pl.color = lv_color_hex(MOKI_INK);
    pl.font  = &moki_fraunces_italic_18;
    lv_area_t la = { (lv_coord_t)(sx + 12), (lv_coord_t)(sy - 12),
                     (lv_coord_t)(sx + 200), (lv_coord_t)(sy + 12) };
    lv_draw_label(draw_ctx, &pl, &la, g_pins[pi].name, NULL);
  }

  // ── FRIENDS (MeshCore-Kontakte mit GPS) ─────────────────────────────────
  int n_contacts = moki_mesh_contact_count();
  uint32_t friends_drawn = 0;
  for (int ci = 0; ci < n_contacts && ci < 32; ci++) {
    float flat, flon;
    uint32_t last_ts;
    if (!moki_mesh_get_contact_loc(ci, &flat, &flon, &last_ts)) continue;
    int sx, sy;
    map_project(&g_viewport, flat, flon, cx_screen, cy_screen, &sx, &sy);
    if (sx < box_x || sx >= box_x + box_w || sy < box_y || sy >= box_y + box_h) continue;

    char fname[24];
    uint8_t key4[4];
    if (!moki_mesh_get_contact(ci, fname, sizeof(fname), key4)) continue;

    // Friend-Marker: voller INK Punkt mit feinem PAPER-Ring
    lv_draw_rect_dsc_t fr_ring;
    lv_draw_rect_dsc_init(&fr_ring);
    fr_ring.bg_color = lv_color_hex(MOKI_PAPER);
    fr_ring.bg_opa = LV_OPA_COVER;
    fr_ring.border_color = lv_color_hex(MOKI_INK);
    fr_ring.border_width = 1;
    fr_ring.radius = LV_RADIUS_CIRCLE;
    lv_area_t fr_a = { (lv_coord_t)(sx - 9), (lv_coord_t)(sy - 9),
                       (lv_coord_t)(sx + 8), (lv_coord_t)(sy + 8) };
    lv_draw_rect(draw_ctx, &fr_ring, &fr_a);

    lv_draw_rect_dsc_t fr_dot;
    lv_draw_rect_dsc_init(&fr_dot);
    fr_dot.bg_color = lv_color_hex(MOKI_INK);
    fr_dot.bg_opa = LV_OPA_COVER;
    fr_dot.radius = LV_RADIUS_CIRCLE;
    lv_area_t fd = { (lv_coord_t)(sx - 4), (lv_coord_t)(sy - 4),
                     (lv_coord_t)(sx + 3), (lv_coord_t)(sy + 3) };
    lv_draw_rect(draw_ctx, &fr_dot, &fd);

    // Name rechts vom Marker
    lv_draw_label_dsc_t fl;
    lv_draw_label_dsc_init(&fl);
    fl.color = lv_color_hex(MOKI_INK);
    fl.font  = &moki_fraunces_italic_18;
    lv_area_t la = { (lv_coord_t)(sx + 13), (lv_coord_t)(sy - 12),
                     (lv_coord_t)(sx + 200), (lv_coord_t)(sy + 12) };
    lv_draw_label(draw_ctx, &fl, &la, fname, NULL);
    friends_drawn++;
  }

  // ── ICH-PUNKT (g_self_pos: GPS oder manuell) ───────────────────────────
  if (g_self_pos.source != 0) {
    int sx, sy;
    map_project(&g_viewport, g_self_pos.lat, g_self_pos.lon,
                cx_screen, cy_screen, &sx, &sy);
    bool in_view = (sx >= box_x && sx < box_x + box_w &&
                    sy >= box_y && sy < box_y + box_h);
    if (in_view) {
      // Halo
      lv_draw_rect_dsc_t halo;
      lv_draw_rect_dsc_init(&halo);
      halo.bg_color = lv_color_hex(MOKI_PAPER);
      halo.bg_opa = LV_OPA_COVER;
      halo.border_color = lv_color_hex(MOKI_INK);
      halo.border_width = 2;
      halo.radius = LV_RADIUS_CIRCLE;
      lv_area_t ah = { (lv_coord_t)(sx-18), (lv_coord_t)(sy-18),
                       (lv_coord_t)(sx+17), (lv_coord_t)(sy+17) };
      lv_draw_rect(draw_ctx, &halo, &ah);

      // Inner dot — solid für GPS, hollow für manual/stale
      lv_draw_rect_dsc_t dot;
      lv_draw_rect_dsc_init(&dot);
      dot.bg_color = lv_color_hex(MOKI_INK);
      dot.bg_opa = LV_OPA_COVER;
      dot.radius = LV_RADIUS_CIRCLE;
      int ds = (g_self_pos.source == 1) ? 7 : 5;
      lv_area_t ad = { (lv_coord_t)(sx-ds), (lv_coord_t)(sy-ds),
                       (lv_coord_t)(sx+ds-1), (lv_coord_t)(sy+ds-1) };
      lv_draw_rect(draw_ctx, &dot, &ad);

      // Label "ich" oder "ich (manuell/letzter fix)"
      const char *lbl = "ich";
      if (g_self_pos.source == 2) lbl = "ich · letzter fix";
      else if (g_self_pos.source == 3) lbl = "ich · manuell";
      lv_draw_label_dsc_t ld;
      lv_draw_label_dsc_init(&ld);
      ld.color = lv_color_hex(MOKI_INK);
      ld.font  = &moki_fraunces_italic_22;
      lv_area_t la = { (lv_coord_t)(sx + 22), (lv_coord_t)(sy - 14),
                       (lv_coord_t)(sx + 240), (lv_coord_t)(sy + 14) };
      lv_draw_label(draw_ctx, &ld, &la, lbl, NULL);
    }
  }

  // ── Status-Pille oben links: GPS-Live-Info ─────────────────────────────
  // Zeigt aktuellen GPS-Status auch wenn ich-Marker schon da ist
  {
    char hint[48];
    if (g_gps.fix) {
      snprintf(hint, sizeof(hint), "GPS · %u sats · ok", (unsigned)g_gps.sats_used);
    } else if (g_self_pos.source != 0) {
      snprintf(hint, sizeof(hint), "ortet · %u sats", (unsigned)g_gps.sats_tracked);
    } else {
      snprintf(hint, sizeof(hint), "kein fix · %u sats", (unsigned)g_gps.sats_tracked);
    }
    lv_draw_rect_dsc_t pill;
    lv_draw_rect_dsc_init(&pill);
    pill.bg_color = lv_color_hex(MOKI_PAPER);
    pill.bg_opa = LV_OPA_COVER;
    pill.border_color = lv_color_hex(MOKI_INK);
    pill.border_width = 1;
    pill.radius = 12;
    lv_area_t pa = { (lv_coord_t)(box_x + 10), (lv_coord_t)(box_y + 10),
                     (lv_coord_t)(box_x + 230), (lv_coord_t)(box_y + 38) };
    lv_draw_rect(draw_ctx, &pill, &pa);

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_hex(MOKI_DARK);
    ld.font  = &moki_jetbrains_mono_18;
    lv_area_t la = { (lv_coord_t)(box_x + 18), (lv_coord_t)(box_y + 14),
                     (lv_coord_t)(box_x + 230), (lv_coord_t)(box_y + 36) };
    lv_draw_label(draw_ctx, &ld, &la, hint, NULL);
  }

  uint32_t t_end = millis();
  Serial.printf("[map] draw · %u drawn, %u skipped, %u cities · %lums\n",
                (unsigned)lines_drawn, (unsigned)lines_skipped,
                (unsigned)cities_drawn, (unsigned long)(t_end - t_start));
}

// Inverse: screen-px → lat/lon. cx_screen/cy_screen sind die Map-Mitte
// in Bildschirm-Koordinaten. Voraussetzung: viewport_compute_scale wurde
// vor diesem Aufruf gerufen (passiert beim Render).
static void map_unproject(int sx, int sy, int cx_screen, int cy_screen,
                          float *out_lat, float *out_lon) {
  float dx = (float)(sx - cx_screen);
  float dy = (float)(sy - cy_screen);
  *out_lon = g_viewport.lon_center + dx / g_viewport.px_per_deg_lon;
  *out_lat = g_viewport.lat_center - dy / g_viewport.px_per_deg_lat;
}

// Pan: Press records start state. Release moves viewport by drag delta,
// OR if it was a long-press without movement, drops a pin.
static void map_press_event_cb(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p; lv_indev_get_point(indev, &p);
  g_drag_start_x = p.x;
  g_drag_start_y = p.y;
  g_drag_start_lat = g_viewport.lat_center;
  g_drag_start_lon = g_viewport.lon_center;
  g_drag_active = true;
  g_press_start_ms = millis();
}

static void map_release_event_cb(lv_event_t *e) {
  if (!g_drag_active) return;
  g_drag_active = false;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p; lv_indev_get_point(indev, &p);
  int dx = p.x - g_drag_start_x;
  int dy = p.y - g_drag_start_y;
  uint32_t held_ms = millis() - g_press_start_ms;

  // LONG-PRESS (>600ms, kaum Bewegung) → Pin setzen
  if (held_ms > 600 && abs(dx) < 12 && abs(dy) < 12) {
    if (!g_map_obj) return;
    lv_area_t coords; lv_obj_get_coords(g_map_obj, &coords);
    int cx_s = coords.x1 + (coords.x2 - coords.x1 + 1) / 2;
    int cy_s = coords.y1 + (coords.y2 - coords.y1 + 1) / 2;
    float plat, plon;
    map_unproject(p.x, p.y, cx_s, cy_s, &plat, &plon);
    char nm[16];
    snprintf(nm, sizeof(nm), "ort %u", (unsigned)(g_pins_n + 1));
    if (pins_add(plat, plon, nm)) {
      Serial.printf("[pin] added '%s' at %.4f,%.4f (held=%lums)\n",
                    nm, plat, plon, (unsigned long)held_ms);
      lv_obj_invalidate(g_map_obj);
    } else {
      Serial.println(F("[pin] add failed (max reached?)"));
    }
    return;
  }

  // Threshold — kleine Bewegung = Tap. Prüfen ob ein Pin getroffen wurde.
  if (abs(dx) < 8 && abs(dy) < 8) {
    // Tap-on-Pin? Reproject Pins auf den aktuellen Screen-Frame
    if (!g_map_obj) return;
    lv_area_t coords; lv_obj_get_coords(g_map_obj, &coords);
    int box_w = coords.x2 - coords.x1 + 1;
    int box_h = coords.y2 - coords.y1 + 1;
    int cx_s = coords.x1 + box_w / 2;
    int cy_s = coords.y1 + box_h / 2;
    viewport_compute_scale(&g_viewport, box_w, box_h);
    for (uint8_t pi = 0; pi < g_pins_n; pi++) {
      int psx, psy;
      map_project(&g_viewport, g_pins[pi].lat, g_pins[pi].lon,
                  cx_s, cy_s, &psx, &psy);
      if (abs(p.x - psx) < 22 && abs(p.y - psy) < 22) {
        g_active_pin = pi;
        Serial.printf("[pin] tap → '%s' (idx=%d)\n", g_pins[pi].name, pi);
        switch_screen(SCR_PIN_DETAIL);
        return;
      }
    }
    return;
  }

  // Drag um (dx, dy) Pixel = Verschiebung um (-dx, -dy) Grad-Anteil
  if (g_viewport.px_per_deg_lat > 0 && g_viewport.px_per_deg_lon > 0) {
    g_viewport.lat_center = g_drag_start_lat + (float)dy / g_viewport.px_per_deg_lat;
    g_viewport.lon_center = g_drag_start_lon - (float)dx / g_viewport.px_per_deg_lon;
    Serial.printf("[map] pan to lat=%.4f lon=%.4f\n",
                  g_viewport.lat_center, g_viewport.lon_center);
    if (g_map_obj) lv_obj_invalidate(g_map_obj);
  }
}

// Zoom-Buttons + Center-on-GPS
static void on_map_zoom_in(lv_event_t *e) {
  if (g_viewport.zoom_idx < MAP_Z_CITY) {
    g_viewport.zoom_idx++;
    Serial.printf("[map] zoom in → %d\n", (int)g_viewport.zoom_idx);
    if (g_map_obj) lv_obj_invalidate(g_map_obj);
    // Reset draggable state
    g_drag_active = false;
  }
}

static void on_map_zoom_out(lv_event_t *e) {
  if (g_viewport.zoom_idx > MAP_Z_COUNTRY) {
    g_viewport.zoom_idx--;
    Serial.printf("[map] zoom out → %d\n", (int)g_viewport.zoom_idx);
    if (g_map_obj) lv_obj_invalidate(g_map_obj);
    g_drag_active = false;
  }
}

static void on_map_center_me(lv_event_t *e) {
  if (g_self_pos.source != 0) {
    g_viewport.lat_center = g_self_pos.lat;
    g_viewport.lon_center = g_self_pos.lon;
    Serial.printf("[map] center on self src=%u lat=%.4f lon=%.4f\n",
                  (unsigned)g_self_pos.source, g_self_pos.lat, g_self_pos.lon);
    if (g_map_obj) lv_obj_invalidate(g_map_obj);
  } else {
    Serial.println(F("[map] center_me: keine self-position bekannt — long-press oder 'h'-Button drücken"));
  }
  g_drag_active = false;
}

static void on_map_set_self_here(lv_event_t *e) {
  // Manuell setzen — auf Viewport-Mitte (= aktueller Karten-Mittelpunkt)
  self_pos_set_manual(g_viewport.lat_center, g_viewport.lon_center);
  Serial.printf("[map] manual self-pos set lat=%.4f lon=%.4f\n",
                g_self_pos.lat, g_self_pos.lon);
  if (g_map_obj) lv_obj_invalidate(g_map_obj);
  g_drag_active = false;
}

// Floating-Button-Helper für die kleine Toolbar in der Map-Ecke
static void make_map_button(lv_obj_t *parent, const char *glyph,
                            int dx, int dy, lv_event_cb_t cb) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, 44, 44);
  lv_obj_set_style_bg_color(b, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
  lv_obj_align(b, LV_ALIGN_TOP_RIGHT, dx, dy);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, glyph);
  lv_obj_set_style_text_font(l, &moki_jetbrains_mono_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_center(l);
  lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void build_map_canvas(lv_obj_t *parent) {
  lv_obj_t *map = lv_obj_create(parent);
  lv_obj_remove_style_all(map);
  lv_obj_set_size(map, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(map, 1);
  lv_obj_set_style_bg_color(map, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(map, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(map, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(map, 1, LV_PART_MAIN);

  g_map_obj = map;

  if (!g_map.ready) {
    lv_obj_t *msg = lv_label_create(map);
    lv_label_set_text(msg, "Karte wird geladen ...");
    lv_obj_set_style_text_font(msg, &moki_fraunces_italic_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(msg, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_center(msg);
    return;
  }

  // Custom-Draw: rendert die Map direkt mit lv_draw_line auf den Display-Buffer
  lv_obj_add_event_cb(map, map_draw_event_cb, LV_EVENT_DRAW_MAIN, NULL);
  // Pan-Gesture: Drag verschiebt Viewport
  lv_obj_add_flag(map, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(map, map_press_event_cb,   LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(map, map_release_event_cb, LV_EVENT_RELEASED, NULL);

  // Floating Toolbar in der oberen rechten Ecke
  make_map_button(map, "+",  -8,  8,   on_map_zoom_in);
  make_map_button(map, "-",  -8,  60,  on_map_zoom_out);
  make_map_button(map, "o",  -8,  112, on_map_center_me);
  make_map_button(map, "h",  -8,  164, on_map_set_self_here);   // "ich bin hier"
}

static void build_nearby_content(lv_obj_t *parent) {
  for (int i = 0; i < SAMPLE_NEARBY_COUNT; i++) {
    const moki_nearby_t *n = &SAMPLE_NEARBY[i];
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(MOKI_MID), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);

    // Avatar — circle with first letter
    lv_obj_t *av = lv_obj_create(row);
    lv_obj_remove_style_all(av);
    lv_obj_set_size(av, 50, 50);
    lv_obj_set_style_bg_color(av, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(av, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_border_width(av, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    char first[2] = { n->name[0], 0 };
    lv_obj_t *fl = lv_label_create(av);
    lv_label_set_text(fl, first);
    lv_obj_set_style_text_font(fl, &moki_fraunces_italic_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(fl, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_center(fl);

    // Text col
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *name = lv_label_create(col);
    lv_label_set_text(name, n->name);
    lv_obj_set_style_text_font(name, &moki_fraunces_regular_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(MOKI_INK), LV_PART_MAIN);

    char mood_line[64];
    snprintf(mood_line, sizeof(mood_line), "mag gerade %s", n->mood);
    lv_obj_t *m = lv_label_create(col);
    lv_label_set_text(m, mood_line);
    lv_obj_set_style_text_font(m, &moki_fraunces_italic_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(m, lv_color_hex(MOKI_DARK), LV_PART_MAIN);

    lv_obj_t *lh = lv_label_create(col);
    lv_label_set_text(lh, n->last_heard);
    lv_obj_set_style_text_font(lh, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lh, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(lh, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_top(lh, 4, LV_PART_MAIN);

    lv_obj_t *dist = lv_label_create(row);
    lv_label_set_text(dist, n->dist);
    lv_obj_set_style_text_font(dist, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(dist, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  }
}

static void build_map_content(lv_obj_t *parent) {
  lv_obj_t *col = lv_obj_create(parent);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_left(col, 24, LV_PART_MAIN);
  lv_obj_set_style_pad_right(col, 24, LV_PART_MAIN);
  lv_obj_set_style_pad_top(col, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(col, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(col, 12, LV_PART_MAIN);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_grow(col, 1);

  // Tab bar
  lv_obj_t *bar = make_tab_bar(col);
  make_tab_button(bar, "karte",       current_map_tab == MAP_MAP,    on_map_tab_clicked, MAP_MAP);
  make_tab_button(bar, "in der nähe", current_map_tab == MAP_NEARBY, on_map_tab_clicked, MAP_NEARBY);

  if (current_map_tab == MAP_MAP) {
    // Header strip — heidelberg + share toggle
    lv_obj_t *head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), 36);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *loc = lv_label_create(head);
    lv_label_set_text(loc, "HEIDELBERG · ~1KM");
    lv_obj_set_style_text_font(loc, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(loc, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(loc, 2, LV_PART_MAIN);

    lv_obj_t *share = lv_label_create(head);
    char buf[32];
    const char *cur = g_settings.share_default;
    const char *cur_label = !strcmp(cur,"off")    ? "AUS"
                          : !strcmp(cur,"hourly") ? "STÜNDLICH"
                                                  : "LIVE";
    snprintf(buf, sizeof(buf), "ICH · %s", cur_label);
    lv_label_set_text(share, buf);
    lv_obj_set_style_text_font(share, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(share, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(share, 2, LV_PART_MAIN);
    lv_obj_add_flag(share, LV_OBJ_FLAG_CLICKABLE);
    static auto cycle_share_cb = [](lv_event_t *) {
      const char *cur = g_settings.share_default;
      const char *next = !strcmp(cur,"off")    ? "hourly"
                       : !strcmp(cur,"hourly") ? "live"
                                               : "off";
      strncpy(g_settings.share_default, next, sizeof(g_settings.share_default)-1);
      g_settings.share_default[sizeof(g_settings.share_default)-1] = 0;
      state_save_settings();
      show_toast("STANDORT-FREIGABE GEWECHSELT");
      switch_screen(SCR_MAP);
    };
    lv_obj_add_event_cb(share, cycle_share_cb, LV_EVENT_CLICKED, NULL);

    build_map_canvas(col);
  } else {
    build_nearby_content(col);
  }
}

static void build_map(void) {
  lv_obj_t *content = build_screen_chrome(lv_scr_act(), 4);
  build_map_content(content);
}

// ============================================================================
// Notes — template picker + read/write editor
// ============================================================================
static void on_note_open(lv_event_t *e) {
  g_active_note = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_active_note >= 0 && g_active_note < g_notes_count) {
    strncpy(g_note_title_buf, g_notes[g_active_note].title, sizeof(g_note_title_buf)-1);
    g_note_title_buf[sizeof(g_note_title_buf)-1] = 0;
    strncpy(g_note_body_buf, g_notes[g_active_note].body, sizeof(g_note_body_buf)-1);
    g_note_body_buf[sizeof(g_note_body_buf)-1] = 0;
    g_note_write = false;
    switch_screen(SCR_NOTE_EDIT);
  }
}
static void on_note_new(lv_event_t *e) {
  switch_screen(SCR_NOTE_NEW);
}
static void on_note_back_to_read(lv_event_t *e) {
  // Persist active note title/body before leaving
  if (g_active_note >= 0 && g_active_note < g_notes_count) {
    strncpy(g_notes[g_active_note].title, g_note_title_buf,
            sizeof(g_notes[g_active_note].title)-1);
    g_notes[g_active_note].title[sizeof(g_notes[g_active_note].title)-1] = 0;
    strncpy(g_notes[g_active_note].body, g_note_body_buf,
            sizeof(g_notes[g_active_note].body)-1);
    g_notes[g_active_note].body[sizeof(g_notes[g_active_note].body)-1] = 0;
    state_save_notes();
  }
  g_active_note = -1;
  current_read_tab = READ_NOTES;
  switch_screen(SCR_READ);
}
static void on_template_picked(lv_event_t *e) {
  const char *tid = (const char *)lv_event_get_user_data(e);
  if (g_notes_count >= MAX_NOTES) return;
  // Find template
  const moki_note_template_t *tpl = NULL;
  for (int i = 0; i < NOTE_TEMPLATE_COUNT; i++)
    if (!strcmp(NOTE_TEMPLATES[i].id, tid)) { tpl = &NOTE_TEMPLATES[i]; break; }
  if (!tpl) return;
  moki_note_t *nt = &g_notes[g_notes_count++];
  strcpy(nt->title, "neu");
  strncpy(nt->body, tpl->body, sizeof(nt->body)-1); nt->body[sizeof(nt->body)-1] = 0;
  strncpy(nt->templ, tpl->id, sizeof(nt->templ)-1); nt->templ[sizeof(nt->templ)-1] = 0;
  nt->folder[0] = 0;
  strcpy(nt->visibility, "private");
  nt->pinned = false;
  nt->updated_at = millis();
  state_save_notes();
  // Open new note in edit mode
  g_active_note = g_notes_count - 1;
  strncpy(g_note_title_buf, nt->title, sizeof(g_note_title_buf)-1);
  g_note_title_buf[sizeof(g_note_title_buf)-1] = 0;
  strncpy(g_note_body_buf, nt->body, sizeof(g_note_body_buf)-1);
  g_note_body_buf[sizeof(g_note_body_buf)-1] = 0;
  g_note_write = true;
  switch_screen(SCR_NOTE_EDIT);
}
static void on_note_pin_toggle(lv_event_t *e) {
  if (g_active_note < 0 || g_active_note >= g_notes_count) return;
  g_notes[g_active_note].pinned = !g_notes[g_active_note].pinned;
  state_save_notes();
  switch_screen(SCR_NOTE_EDIT);
}
static void on_note_delete(lv_event_t *e) {
  if (g_active_note < 0 || g_active_note >= g_notes_count) return;
  for (int i = g_active_note; i < g_notes_count - 1; i++)
    g_notes[i] = g_notes[i+1];
  g_notes_count--;
  state_save_notes();
  g_active_note = -1;
  current_read_tab = READ_NOTES;
  switch_screen(SCR_READ);
}
static void on_note_mode_toggle(lv_event_t *e) {
  g_note_write = !g_note_write;
  switch_screen(SCR_NOTE_EDIT);
}

// Strip simple inline markdown markers (**bold** and *italic*) from a line.
// We don't have proper inline runs in LVGL labels, so we just strip the
// punctuation and let the line read cleanly.
static String strip_inline_md(const String &s) {
  String out;
  out.reserve(s.length());
  int from = 0;
  while (from < (int)s.length()) {
    int p_b = s.indexOf("**", from);
    int p_i = s.indexOf('*', from);
    int p   = (p_b >= 0 && (p_i < 0 || p_b <= p_i)) ? p_b : p_i;
    if (p < 0) { out += s.substring(from); break; }
    out += s.substring(from, p);
    int mlen  = (p == p_b) ? 2 : 1;
    int close = s.indexOf((p == p_b) ? "**" : "*", p + mlen);
    if (close < 0) { out += s.substring(p); break; }
    out += s.substring(p + mlen, close);
    from = close + mlen;
  }
  return out;
}

// Rough markdown line renderer — appends labels for each line into a column.
// Subset: # H1, ## H2, ### H3, - bullet, > quote, --- divider, plain text.
static void render_markdown_into(lv_obj_t *parent, const char *text) {
  String src = text ? String(text) : String("");
  int from = 0;
  while (from <= src.length()) {
    int nl = src.indexOf('\n', from);
    String line = (nl < 0) ? src.substring(from) : src.substring(from, nl);
    from = (nl < 0) ? src.length() + 1 : nl + 1;

    if (line.length() == 0) {
      lv_obj_t *sp = lv_obj_create(parent);
      lv_obj_remove_style_all(sp);
      lv_obj_set_size(sp, 1, 8);
      lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
      continue;
    }

    if (line == "---") {
      lv_obj_t *hr = lv_obj_create(parent);
      lv_obj_remove_style_all(hr);
      lv_obj_set_size(hr, LV_PCT(100), 1);
      lv_obj_set_style_bg_color(hr, lv_color_hex(MOKI_MID), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, LV_PART_MAIN);
      continue;
    }

    const lv_font_t *font = &moki_fraunces_italic_22;
    String body_line = line;
    int color = MOKI_INK;
    if (line.startsWith("# ")) {
      font = &moki_fraunces_italic_36;
      body_line = line.substring(2);
    } else if (line.startsWith("## ")) {
      font = &moki_fraunces_regular_36;
      body_line = line.substring(3);
    } else if (line.startsWith("### ")) {
      font = &moki_fraunces_italic_28;
      body_line = line.substring(4);
    } else if (line.startsWith("- ")) {
      font = &moki_fraunces_italic_22;
      body_line = String("·  ") + strip_inline_md(line.substring(2));
      lv_obj_t *l = lv_label_create(parent);
      lv_label_set_text(l, body_line.c_str());
      lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
      lv_obj_set_style_text_color(l, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_set_width(l, LV_PCT(100));
      lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
      continue;
    } else if (line.startsWith("> ")) {
      font = &moki_fraunces_italic_22;
      body_line = line.substring(2);
      color = MOKI_DARK;
    }

    String stripped = strip_inline_md(body_line);
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, stripped.c_str());
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  }
}

static void build_note_new(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 16, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 14, LV_PART_MAIN);

  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, LV_PCT(100), 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_note_back_to_read, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← ZURÜCK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);

  lv_obj_t *kicker = lv_label_create(scr);
  lv_label_set_text(kicker, "VORLAGE");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "wie soll sie beginnen?");
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  // Template chips — full-width rows for readable labels
  for (int i = 0; i < NOTE_TEMPLATE_COUNT; i++) {
    const moki_note_template_t *tpl = &NOTE_TEMPLATES[i];
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 70);
    lv_obj_set_style_bg_color(row, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_template_picked, LV_EVENT_CLICKED, (void *)tpl->id);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, tpl->label);
    lv_obj_set_style_text_font(l, &moki_fraunces_regular_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  }
}

// Forward — keyboard callback shared with note editor
static void note_key_event(const char *key);
static void on_note_key_clicked(lv_event_t *e) {
  note_key_event((const char *)lv_event_get_user_data(e));
}

static void build_note_edit(void) {
  if (g_active_note < 0 || g_active_note >= g_notes_count) {
    switch_screen(SCR_READ); return;
  }

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

  // Header strip — back / mode toggle / pin / delete
  lv_obj_t *hdr = lv_obj_create(scr);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_size(hdr, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(hdr, 20, LV_PART_MAIN);
  lv_obj_set_style_pad_right(hdr, 20, LV_PART_MAIN);
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(hdr, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  auto make_btn = [](lv_obj_t *parent, const char *text, lv_event_cb_t cb) {
    lv_obj_t *b = lv_label_create(parent);
    lv_label_set_text(b, text);
    lv_obj_set_style_text_font(b, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(b, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(b, 2, LV_PART_MAIN);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
  };
  make_btn(hdr, "ZURÜCK", on_note_back_to_read);
  make_btn(hdr, g_notes[g_active_note].pinned ? "ANGEPINNT" : "ANPINNEN", on_note_pin_toggle);
  make_btn(hdr, g_note_write ? "LESEN" : "SCHREIBEN", on_note_mode_toggle);
  make_btn(hdr, "LÖSCHEN", on_note_delete);

  // Title — always editable inline; for now plain label of buf
  lv_obj_t *title_box = lv_obj_create(scr);
  lv_obj_remove_style_all(title_box);
  lv_obj_set_size(title_box, LV_PCT(100), 64);
  lv_obj_set_style_pad_left(title_box, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(title_box, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(title_box, 12, LV_PART_MAIN);

  g_note_title_label = lv_label_create(title_box);
  lv_label_set_text(g_note_title_label, g_note_title_buf);
  lv_obj_set_style_text_font(g_note_title_label, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(g_note_title_label, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  // Body — read mode renders markdown, write mode shows raw + keyboard
  lv_obj_t *body = lv_obj_create(scr);
  lv_obj_remove_style_all(body);
  lv_obj_set_width(body, LV_PCT(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_style_pad_left(body, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(body, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(body, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(body, 16, LV_PART_MAIN);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 4, LV_PART_MAIN);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

  if (!g_note_write) {
    render_markdown_into(body, g_note_body_buf);
  } else {
    g_note_body_label = lv_label_create(body);
    lv_label_set_text(g_note_body_label, g_note_body_buf);
    lv_obj_set_style_text_font(g_note_body_label, &moki_fraunces_italic_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_note_body_label, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_width(g_note_body_label, LV_PCT(100));
    lv_label_set_long_mode(g_note_body_label, LV_LABEL_LONG_WRAP);

    // Format toolbar — H1 / H2 / bullet / quote / divider / newline
    lv_obj_t *tools = lv_obj_create(scr);
    lv_obj_remove_style_all(tools);
    lv_obj_set_size(tools, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(tools, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tools, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(tools, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(tools, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
    lv_obj_set_style_border_width(tools, 1, LV_PART_MAIN);
    lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tools, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto add_tool = [&](const char *txt, const char *kk) {
      lv_obj_t *b = lv_label_create(tools);
      lv_label_set_text(b, txt);
      lv_obj_set_style_text_font(b, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(b, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(b, on_note_key_clicked, LV_EVENT_CLICKED, (void *)kk);
    };
    add_tool("H1",  "TOOL_H1");
    add_tool("H2",  "TOOL_H2");
    add_tool("LIST", "TOOL_BUL");
    add_tool("QUOT", "TOOL_QUO");
    add_tool("HR",   "TOOL_HR");
    add_tool("NEU",  "TOOL_NL");

    // Keyboard
    lv_obj_t *kb = lv_obj_create(scr);
    lv_obj_remove_style_all(kb);
    lv_obj_set_size(kb, LV_PCT(100), 240);
    lv_obj_set_style_bg_color(kb, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_top(kb, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(kb, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(kb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(kb, 4, LV_PART_MAIN);

    static const char *r1[] = {"q","w","e","r","t","z","u","i","o","p"};
    static const char *r2[] = {"a","s","d","f","g","h","j","k","l","ä"};
    static const char *r3[] = {"y","x","c","v","b","n","m","ö","ü","ß"};
    auto build_row_local = [&](const char *const *keys) {
      lv_obj_t *row = lv_obj_create(kb);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, LV_PCT(100), 56);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
      for (int i = 0; i < 10; i++) {
        lv_obj_t *btn = lv_obj_create(row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 48, 50);
        lv_obj_set_style_bg_color(btn, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 2, LV_PART_MAIN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, on_note_key_clicked, LV_EVENT_CLICKED, (void *)keys[i]);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, keys[i]);
        lv_obj_set_style_text_font(l, &moki_fraunces_regular_36, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(MOKI_INK), LV_PART_MAIN);
        lv_obj_center(l);
      }
    };
    build_row_local(r1); build_row_local(r2); build_row_local(r3);

    lv_obj_t *r4 = lv_obj_create(kb);
    lv_obj_remove_style_all(r4);
    lv_obj_set_size(r4, LV_PCT(100), 56);
    lv_obj_set_flex_flow(r4, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r4, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r4, 6, LV_PART_MAIN);

    auto big_btn = [&](const char *txt, const char *kk, int w) {
      lv_obj_t *btn = lv_obj_create(r4);
      lv_obj_remove_style_all(btn);
      lv_obj_set_size(btn, w, 50);
      lv_obj_set_style_bg_color(btn, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(btn, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(btn, 2, LV_PART_MAIN);
      lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(btn, on_note_key_clicked, LV_EVENT_CLICKED, (void *)kk);
      lv_obj_t *l = lv_label_create(btn);
      lv_label_set_text(l, txt);
      lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(l, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_center(l);
    };
    big_btn("ENTF", "BACK", 80);
    big_btn("LEERZEICHEN", "SPACE", 280);
    big_btn(".", ".", 60);
  }
}

static void note_key_event(const char *key) {
  size_t len = strlen(g_note_body_buf);
  auto append = [&](const char *s) {
    size_t klen = strlen(s);
    size_t cur = strlen(g_note_body_buf);
    if (cur + klen + 1 < sizeof(g_note_body_buf)) {
      memcpy(g_note_body_buf + cur, s, klen);
      g_note_body_buf[cur + klen] = 0;
    }
  };
  if (!strcmp(key, "BACK")) {
    if (len > 0) {
      do { len--; } while (len > 0 && (((unsigned char)g_note_body_buf[len]) & 0xC0) == 0x80);
      g_note_body_buf[len] = 0;
    }
  } else if (!strcmp(key, "SPACE")) append(" ");
  else if (!strcmp(key, "TOOL_H1"))  append("\n# ");
  else if (!strcmp(key, "TOOL_H2"))  append("\n## ");
  else if (!strcmp(key, "TOOL_BUL")) append("\n- ");
  else if (!strcmp(key, "TOOL_QUO")) append("\n> ");
  else if (!strcmp(key, "TOOL_HR"))  append("\n---\n");
  else if (!strcmp(key, "TOOL_NL"))  append("\n");
  else append(key);

  if (g_note_body_label)
    lv_label_set_text(g_note_body_label, g_note_body_buf);
}

// ============================================================================
// SETTINGS — sync interval, share-mode default
// ============================================================================
static void on_settings_back(lv_event_t *e) { switch_screen(SCR_PROFILE); }
static void on_sync_interval_picked(lv_event_t *e) {
  g_settings.sync_interval_min = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  state_save_settings();
  show_toast("EINSTELLUNG GESPEICHERT");
  switch_screen(SCR_SETTINGS);
}
static void on_share_default_picked(lv_event_t *e) {
  const char *s = (const char *)lv_event_get_user_data(e);
  strncpy(g_settings.share_default, s, sizeof(g_settings.share_default)-1);
  g_settings.share_default[sizeof(g_settings.share_default)-1] = 0;
  state_save_settings();
  show_toast("EINSTELLUNG GESPEICHERT");
  switch_screen(SCR_SETTINGS);
}

void build_settings(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 16, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 14, LV_PART_MAIN);

  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, LV_PCT(100), 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_settings_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← ZURÜCK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);

  lv_obj_t *kicker = lv_label_create(scr);
  lv_label_set_text(kicker, "EINSTELLUNGEN");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "wie soll moki sich verhalten?");
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_width(title, LV_PCT(100));
  lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);

  // Sync interval picker
  lv_obj_t *sl = lv_label_create(scr);
  lv_label_set_text(sl, "SYNC-INTERVALL");
  lv_obj_set_style_text_font(sl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sl, 3, LV_PART_MAIN);

  lv_obj_t *sync_strip = lv_obj_create(scr);
  lv_obj_remove_style_all(sync_strip);
  lv_obj_set_size(sync_strip, LV_PCT(100), 50);
  lv_obj_set_flex_flow(sync_strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(sync_strip, 8, LV_PART_MAIN);

  static const uint8_t INTERVALS[] = { 5, 15, 30, 60 };
  static const char *INTERVAL_LABELS[] = { "5 MIN", "15 MIN", "30 MIN", "60 MIN" };
  for (int i = 0; i < 4; i++) {
    bool active = (g_settings.sync_interval_min == INTERVALS[i]);
    lv_obj_t *c = lv_obj_create(sync_strip);
    lv_obj_remove_style_all(c);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_style_bg_color(c, lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, lv_color_hex(active ? MOKI_INK : MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, on_sync_interval_picked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)INTERVALS[i]);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, INTERVAL_LABELS[i]);
    lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(l,
        lv_color_hex(active ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(l, 2, LV_PART_MAIN);
  }

  lv_obj_t *hint = lv_label_create(scr);
  uint8_t iv = g_settings.sync_interval_min;
  const char *bat_hint = iv ==  5 ? "ca. 3 tage akku" :
                          iv == 15 ? "ca. 1 woche akku" :
                          iv == 30 ? "ca. 2-4 wochen akku" :
                                     "ca. 4-6 wochen akku";
  char hint_buf[80];
  snprintf(hint_buf, sizeof(hint_buf), "%s · slow tech ist absicht.", bat_hint);
  lv_label_set_text(hint, hint_buf);
  lv_obj_set_style_text_font(hint, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_width(hint, LV_PCT(100));
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

  // Share default
  lv_obj_t *sl2 = lv_label_create(scr);
  lv_label_set_text(sl2, "STANDORT-FREIGABE");
  lv_obj_set_style_text_font(sl2, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl2, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sl2, 3, LV_PART_MAIN);

  lv_obj_t *share_strip = lv_obj_create(scr);
  lv_obj_remove_style_all(share_strip);
  lv_obj_set_size(share_strip, LV_PCT(100), 50);
  lv_obj_set_flex_flow(share_strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(share_strip, 8, LV_PART_MAIN);

  static const char *SHARE_IDS[] = { "off", "hourly", "live" };
  static const char *SHARE_LABELS[] = { "AUS", "STÜNDLICH", "LIVE" };
  for (int i = 0; i < 3; i++) {
    bool active = !strcmp(g_settings.share_default, SHARE_IDS[i]);
    lv_obj_t *c = lv_obj_create(share_strip);
    lv_obj_remove_style_all(c);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_style_bg_color(c, lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, lv_color_hex(active ? MOKI_INK : MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, on_share_default_picked, LV_EVENT_CLICKED, (void *)SHARE_IDS[i]);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, SHARE_LABELS[i]);
    lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(active ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(l, 2, LV_PART_MAIN);
  }

  // LoRa TX-armed toggle
  lv_obj_t *ll = lv_label_create(scr);
  lv_label_set_text(ll, "LORA SENDEN");
  lv_obj_set_style_text_font(ll, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(ll, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(ll, 3, LV_PART_MAIN);

  lv_obj_t *lora_strip = lv_obj_create(scr);
  lv_obj_remove_style_all(lora_strip);
  lv_obj_set_size(lora_strip, LV_PCT(100), 50);
  lv_obj_set_flex_flow(lora_strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(lora_strip, 8, LV_PART_MAIN);

  static const bool LORA_VAL[] = { false, true };
  static const char *LORA_LABELS[] = { "AUS · RX-ONLY", "ANTENNE DRAN" };
  static auto on_lora_picked = [](lv_event_t *e) {
    g_settings.lora_tx_armed = (bool)(intptr_t)lv_event_get_user_data(e);
    state_save_settings();
    show_toast(g_settings.lora_tx_armed ? "TX FREIGEGEBEN" : "RX-ONLY MODUS");
    switch_screen(SCR_SETTINGS);
  };
  for (int i = 0; i < 2; i++) {
    bool active = (g_settings.lora_tx_armed == LORA_VAL[i]);
    lv_obj_t *c = lv_obj_create(lora_strip);
    lv_obj_remove_style_all(c);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_style_bg_color(c, lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, lv_color_hex(active ? MOKI_INK : MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, on_lora_picked, LV_EVENT_CLICKED, (void *)(intptr_t)LORA_VAL[i]);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, LORA_LABELS[i]);
    lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(l,
        lv_color_hex(active ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
  }

  lv_obj_t *lhint = lv_label_create(scr);
  lv_label_set_text(lhint,
    "ohne antenne kann der lora-chip beim senden schaden nehmen.\n"
    "erst antenne anschrauben, dann freigeben.");
  lv_obj_set_style_text_font(lhint, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(lhint, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_width(lhint, LV_PCT(100));
  lv_label_set_long_mode(lhint, LV_LABEL_LONG_WRAP);

  // ── Auto-Sleep (M3) ─────────────────────────────────────────────────
  lv_obj_t *asl = lv_label_create(scr);
  lv_label_set_text(asl, "AUTO-SCHLAF NACH IDLE");
  lv_obj_set_style_text_font(asl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(asl, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(asl, 3, LV_PART_MAIN);

  lv_obj_t *as_strip = lv_obj_create(scr);
  lv_obj_remove_style_all(as_strip);
  lv_obj_set_size(as_strip, LV_PCT(100), 50);
  lv_obj_set_flex_flow(as_strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(as_strip, 8, LV_PART_MAIN);

  static const uint8_t AS_VAL[]    = { 0,    1,    5,    15 };
  static const char *AS_LABEL[]    = { "AUS","1 MIN","5 MIN","15 MIN" };
  static auto on_as_picked = [](lv_event_t *e) {
    uint8_t v = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    g_settings.auto_sleep_min = v;
    state_save_settings();
    mark_activity();
    show_toast(v == 0 ? "AUTO-SCHLAF AUS" : "AUTO-SCHLAF AKTIV");
    switch_screen(SCR_SETTINGS);
  };
  for (int i = 0; i < 4; i++) {
    bool active = (g_settings.auto_sleep_min == AS_VAL[i]);
    lv_obj_t *c = lv_obj_create(as_strip);
    lv_obj_remove_style_all(c);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_style_bg_color(c, lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, lv_color_hex(active ? MOKI_INK : MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, on_as_picked, LV_EVENT_CLICKED, (void *)(intptr_t)AS_VAL[i]);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, AS_LABEL[i]);
    lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(l,
        lv_color_hex(active ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
  }

  // ── Channel + Identity (M7) ────────────────────────────────────────
  lv_obj_t *chl = lv_label_create(scr);
  lv_label_set_text(chl, "MESH-KANAL");
  lv_obj_set_style_text_font(chl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(chl, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(chl, 3, LV_PART_MAIN);

  char chan_buf[80];
  snprintf(chan_buf, sizeof(chan_buf), "%s · psk %.6s…",
           g_settings.mesh_channel_name, g_settings.mesh_channel_psk_b64);
  lv_obj_t *ch_val = lv_label_create(scr);
  lv_label_set_text(ch_val, chan_buf);
  lv_obj_set_style_text_font(ch_val, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(ch_val, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_width(ch_val, LV_PCT(100));

  // Identity: first 4 hex bytes of secret as a stable visual fingerprint.
  char id_buf[80];
  if (g_identity_ready) {
    snprintf(id_buf, sizeof(id_buf), "ID · %02x%02x%02x%02x",
             g_identity_secret[0], g_identity_secret[1],
             g_identity_secret[2], g_identity_secret[3]);
  } else {
    snprintf(id_buf, sizeof(id_buf), "ID · noch nicht generiert");
  }
  lv_obj_t *id_lbl = lv_label_create(scr);
  lv_label_set_text(id_lbl, id_buf);
  lv_obj_set_style_text_font(id_lbl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(id_lbl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(id_lbl, 2, LV_PART_MAIN);

  // Footer / build info
  lv_obj_t *foot = lv_label_create(scr);
  lv_label_set_text(foot, "MOKI · BUILD VOM 30. APRIL · v0.5");
  lv_obj_set_style_text_font(foot, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(foot, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(foot, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_top(foot, 16, LV_PART_MAIN);
}

// ============================================================================
// CHAT DETAIL — single conversation thread (read-only, sample messages)
// ============================================================================
typedef struct { const char *from; const char *text; const char *ts; } chat_msg_t;

static const chat_msg_t MSGS_LINA[] = {
  { "lina", "hey", "vor 22 min" },
  { "lina", "magst du samstag tanzen?", "vor 8 min" },
};
static const chat_msg_t MSGS_LESEKREIS[] = {
  { "tom",  "hat jemand das buch durch?", "vor 3h" },
  { "lina", "nein noch nicht", "vor 2h" },
  { "lina", "walden kap 4 bis freitag ok?", "vor 2h" },
};
static const chat_msg_t MSGS_RHEIN[] = {
  { "unbekannt · HDB-22ee", "empfehlung café mit guter milch?", "vor 3h" },
  { "juno",                 "café frieda", "vor 2h" },
  { "unbekannt · HDB-88dd", "jemand heute abend am neckar?", "vor 45 min" },
};

extern void open_lora_compose(void);
static void on_lora_send_clicked(lv_event_t *e) { open_lora_compose(); }

// Forward decls — actual definitions live in the LoRa section below.
static void lora_apply_preset(uint8_t preset);
extern void show_toast(const char *msg);
static void enter_deep_sleep(uint32_t wake_after_seconds);
extern void open_lora_compose(void);
extern int  g_dm_target_idx;

static void on_lora_preset_picked(lv_event_t *e) {
  uint8_t preset = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  if (preset >= LORA_PRESET_COUNT) return;
  g_settings.lora_preset = preset;
  state_save_settings();
  if (g_lora_ready) lora_apply_preset(preset);
  // Don't wipe last_rx — the ring buffer still has those packets and the
  // "vor Xs" age is genuinely informative across preset switches.
  char msg[32];
  snprintf(msg, sizeof(msg), "preset · %s", LORA_PRESET_LABELS[preset]);
  show_toast(msg);
}

// Channel chip click — switch active channel for outgoing messages.
static void on_mesh_channel_picked(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_num_channels) return;
  g_settings.mesh_active_channel = (uint8_t)idx;
  state_save_settings();
  char msg[32];
  snprintf(msg, sizeof(msg), "kanal · %s", g_channels[idx].name);
  show_toast(msg);
  switch_screen(SCR_CHAT_DETAIL);
}

// Advanced/preset toggle — lets power users still reach the
// moki/narrow/legacy/mtast preset picker. Default users never see it.
static bool g_show_lora_advanced = false;
static void on_lora_advanced_toggle(lv_event_t *e) {
  g_show_lora_advanced = !g_show_lora_advanced;
  switch_screen(SCR_CHAT_DETAIL);
}

// ----------------------------------------------------------------------------
// Reader screen — Vollbild, kein Tab-Bar, kein Dock. Eigener Header mit
// "← Buchtitel" links, darunter der Text, unten die Seiten-Navi.
// ----------------------------------------------------------------------------
static void on_reader_back(lv_event_t *e) {
  current_read_tab = READ_BOOK;
  switch_screen(SCR_READ);
}

static void reader_persist_page(void) {
  if (g_book_active_idx < 0) {
    state_save_book_page();
  } else {
    char k[16]; snprintf(k, sizeof(k), "book_p_%d", g_book_active_idx);
    g_prefs.begin("moki", false);
    g_prefs.putUInt(k, (uint32_t)g_book_page);
    g_prefs.end();
  }
}

static void on_reader_prev(lv_event_t *e) {
  if (g_book_page > 0) {
    g_book_page--;
    reader_persist_page();
    switch_screen(SCR_READER);
  }
}

static void on_reader_next(lv_event_t *e) {
  size_t total = book_total_length();
  int total_pages = (int)((total + BOOK_CHARS_PER_PAGE - 1) / BOOK_CHARS_PER_PAGE);
  if (g_book_page < total_pages - 1) {
    g_book_page++;
    reader_persist_page();
    switch_screen(SCR_READER);
  }
}

// ── Pin-Detail-Screen ────────────────────────────────────────────────────
// Zeigt einen Pin (Name, Notiz, Koords). Buttons: löschen + zurück. Rename
// und Notiz-Editieren via Console-Befehle (pin_rename / pin_note) bis
// keyboard-driven Editor in eigener Iteration kommt.
static void on_pin_back(lv_event_t *e) {
  g_active_pin = -1;
  switch_screen(SCR_MAP);
}

static void on_pin_delete(lv_event_t *e) {
  if (g_active_pin >= 0 && g_active_pin < (int)g_pins_n) {
    pins_delete(g_active_pin);
  }
  g_active_pin = -1;
  switch_screen(SCR_MAP);
}

static void on_pin_open_rename(lv_event_t *e) {
  if (g_active_pin >= 0) switch_screen(SCR_PIN_RENAME);
}
static void on_pin_open_note(lv_event_t *e) {
  if (g_active_pin >= 0) switch_screen(SCR_PIN_NOTE);
}
// Send-to-all: pragmatisches v0 — sendet an alle bekannten Kontakte. v1
// kommt mit Picker (eigener Screen mit Kontaktliste).
static void on_pin_share_all(lv_event_t *e) {
  if (g_active_pin < 0 || g_active_pin >= (int)g_pins_n) return;
  if (!g_settings.lora_tx_armed) {
    Serial.println(F("[pin] tx not armed"));
    return;
  }
  const moki_pin_t *p = &g_pins[g_active_pin];
  String payload = pin_share_format(p);
  int n = moki_mesh_contact_count();
  int sent = 0;
  for (int i = 0; i < n; i++) {
    if (moki_mesh_dm(i, payload.c_str())) sent++;
  }
  Serial.printf("[pin] shared '%s' to %d/%d contacts\n",
                p->name, sent, n);
}

// Generic: tasten-edit screen with lv_textarea + lv_keyboard
//   title  — kicker text shown above (z.B. "ORT UMBENENNEN")
//   initial — initialer Textarea-Inhalt
//   on_save_cb — wird aufgerufen mit textarea-text, schreibt zurück
//   back_to — Screen, zu dem nach OK / Cancel zurückgekehrt wird
static lv_obj_t *g_pin_edit_ta = nullptr;
static char     g_pin_edit_initial[128] = "";
static const char *g_pin_edit_title = "";

static void on_pin_edit_save(lv_event_t *e) {
  if (g_active_pin < 0 || !g_pin_edit_ta) return;
  const char *txt = lv_textarea_get_text(g_pin_edit_ta);
  if (!txt) txt = "";
  // Welcher Edit ist gerade offen — basierend auf Title
  if (strstr(g_pin_edit_title, "UMBENENNEN")) {
    pins_rename(g_active_pin, txt);
  } else if (strstr(g_pin_edit_title, "NOTIZ")) {
    pins_set_note(g_active_pin, txt);
  }
  switch_screen(SCR_PIN_DETAIL);
}
static void on_pin_edit_cancel(lv_event_t *e) {
  switch_screen(SCR_PIN_DETAIL);
}

// Tastatur-Event: OK = Save, Cancel = back
static void on_pin_edit_kb_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY)  { on_pin_edit_save(NULL); }
  if (code == LV_EVENT_CANCEL) { on_pin_edit_cancel(NULL); }
}

static void build_pin_edit_screen(const char *title, const char *initial) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

  // Header strip — back / kicker
  lv_obj_t *hdr = lv_obj_create(scr);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_size(hdr, LV_PCT(100), 56);
  lv_obj_set_style_pad_left(hdr, 24, LV_PART_MAIN);
  lv_obj_set_style_pad_right(hdr, 24, LV_PART_MAIN);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *back = lv_obj_create(hdr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, 100, 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_pin_edit_cancel, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← ZURÜCK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_add_flag(bl, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t *kicker = lv_label_create(hdr);
  lv_label_set_text(kicker, title);
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  // Save button on the right
  lv_obj_t *save = lv_obj_create(hdr);
  lv_obj_remove_style_all(save);
  lv_obj_set_size(save, 100, 36);
  lv_obj_set_style_bg_color(save, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(save, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flex_flow(save, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(save, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(save, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(save, on_pin_edit_save, LV_EVENT_CLICKED, NULL);
  lv_obj_t *sl = lv_label_create(save);
  lv_label_set_text(sl, "OK");
  lv_obj_set_style_text_font(sl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sl, 3, LV_PART_MAIN);
  lv_obj_add_flag(sl, LV_OBJ_FLAG_EVENT_BUBBLE);

  // Textarea
  lv_obj_t *ta = lv_textarea_create(scr);
  lv_obj_set_size(ta, LV_PCT(100), 120);
  lv_obj_set_style_text_font(ta, &moki_fraunces_italic_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(ta, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_color(ta, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_border_width(ta, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ta, 16, LV_PART_MAIN);
  lv_textarea_set_one_line(ta, false);
  lv_textarea_set_text(ta, initial ? initial : "");
  g_pin_edit_ta = ta;
  g_pin_edit_title = title;

  // Keyboard
  lv_obj_t *kb = lv_keyboard_create(scr);
  lv_keyboard_set_textarea(kb, ta);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_size(kb, LV_PCT(100), 380);
  lv_obj_set_style_text_font(kb, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_add_event_cb(kb, on_pin_edit_kb_event, LV_EVENT_ALL, NULL);
}

// ── OTA-Update-Screen (M8.8) ─────────────────────────────────────────────
// Vollbild "Update läuft …" Status. Zeigt Progress, blockt während download
// und triggert reboot bei Erfolg.
#ifdef MOKI_WIFI
static lv_obj_t *g_ota_status_label = nullptr;

static void ota_ui_status(const char *txt) {
  if (g_ota_status_label) {
    lv_label_set_text(g_ota_status_label, txt);
    // LVGL rendern damit der User es sieht bevor ota_pull blockiert
    for (int i = 0; i < 5; i++) { lv_timer_handler(); delay(20); }
  }
  Serial.printf("[ota_ui] %s\n", txt);
}

static void on_ota_back(lv_event_t *e) {
  switch_screen(SCR_WIFI);
}

static void on_ota_start(lv_event_t *e) {
  if (!g_wifi.connected) {
    ota_ui_status("kein WLAN — bitte zuerst verbinden");
    return;
  }
  if (!g_ota_url[0]) {
    ota_ui_status("keine update-url konfiguriert");
    return;
  }
  ota_ui_status("verbinde mit github …");
  bool ok = ota_pull(g_ota_url);
  if (ok) {
    ota_ui_status("update OK · neustart …");
    // ota_pull macht ESP.restart() bei Erfolg, hier sollten wir nie ankommen
  } else {
    ota_ui_status("update fehlgeschlagen — versuch's nochmal");
  }
}

void build_ota_screen(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 32, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 32, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 18, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 18, LV_PART_MAIN);

  // Header back
  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, LV_PCT(100), 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_ota_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← WLAN");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);

  // Kicker
  lv_obj_t *kicker = lv_label_create(scr);
  lv_label_set_text(kicker, "FIRMWARE-UPDATE");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  // Title
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "neueste version aus dem netz holen");
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_width(title, LV_PCT(100));
  lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);

  // Quelle
  lv_obj_t *src = lv_label_create(scr);
  char b[180];
  snprintf(b, sizeof(b), "Quelle:\n%s", g_ota_url);
  lv_label_set_text(src, b);
  lv_obj_set_style_text_font(src, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(src, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_width(src, LV_PCT(100));
  lv_label_set_long_mode(src, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(src, 4, LV_PART_MAIN);

  // Status (wird live aktualisiert)
  lv_obj_t *status = lv_label_create(scr);
  if (!g_wifi.connected) {
    lv_label_set_text(status, "→ kein WLAN. erst Profil → WLAN verbinden.");
    lv_obj_set_style_text_color(status, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  } else {
    lv_label_set_text(status, "bereit. drück »JETZT UPDATEN«.");
    lv_obj_set_style_text_color(status, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  }
  lv_obj_set_style_text_font(status, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_width(status, LV_PCT(100));
  lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
  lv_obj_set_flex_grow(status, 1);
  g_ota_status_label = status;

  // Update-Button
  lv_obj_t *btn = lv_obj_create(scr);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, LV_PCT(100), 64);
  lv_obj_set_style_bg_color(btn, lv_color_hex(g_wifi.connected ? MOKI_INK : MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(btn, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (g_wifi.connected) {
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_ota_start, LV_EVENT_CLICKED, NULL);
  }
  lv_obj_t *bt = lv_label_create(btn);
  lv_label_set_text(bt, "JETZT UPDATEN");
  lv_obj_set_style_text_font(bt, &moki_jetbrains_mono_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(bt, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bt, 3, LV_PART_MAIN);
  lv_obj_add_flag(bt, LV_OBJ_FLAG_EVENT_BUBBLE);
}
#else
void build_ota_screen(void) { switch_screen(SCR_PROFILE); }
#endif

// ── WiFi-Setup-Screens (M8.8) ────────────────────────────────────────────
#ifdef MOKI_WIFI
// Scratch state für Picked-SSID + PSK-Edit. Dual-Mode: scan-pick ODER manueller
// SSID-Eintrag (für den Fall dass Scan nicht klappt — DRAM-Knappheit).
typedef enum { WIFI_EDIT_PSK = 0, WIFI_EDIT_SSID = 1 } wifi_edit_mode_t;
static char             g_wifi_pick_ssid[33] = "";
static char             g_wifi_pick_psk[65] = "";
static wifi_edit_mode_t g_wifi_edit_mode = WIFI_EDIT_PSK;
static lv_obj_t *g_wifi_psk_ta = nullptr;

static void on_wifi_back(lv_event_t *e) {
  switch_screen(SCR_PROFILE);
}

static void on_wifi_psk_back(lv_event_t *e) {
  switch_screen(SCR_WIFI);
}

static void on_wifi_psk_save(lv_event_t *e) {
  if (!g_wifi_psk_ta) {
    switch_screen(SCR_WIFI); return;
  }
  const char *txt = lv_textarea_get_text(g_wifi_psk_ta);
  if (!txt) txt = "";

  if (g_wifi_edit_mode == WIFI_EDIT_SSID) {
    // SSID gerade eingegeben — wechsel zu PSK-Edit
    strncpy(g_wifi_pick_ssid, txt, sizeof(g_wifi_pick_ssid) - 1);
    g_wifi_pick_ssid[sizeof(g_wifi_pick_ssid) - 1] = 0;
    g_wifi_edit_mode = WIFI_EDIT_PSK;
    switch_screen(SCR_WIFI_PSK);
    return;
  }

  // PSK-Edit: jetzt connecten
  if (!g_wifi_pick_ssid[0]) {
    switch_screen(SCR_WIFI); return;
  }
  wifi_set_creds(g_wifi_pick_ssid, txt);
  switch_screen(SCR_WIFI);
}

static void on_wifi_psk_kb_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY)  { on_wifi_psk_save(NULL); }
  if (code == LV_EVENT_CANCEL) { on_wifi_psk_back(NULL); }
}

static void on_wifi_pick(lv_event_t *e) {
  const char *ssid = (const char *)lv_event_get_user_data(e);
  if (!ssid) return;
  strncpy(g_wifi_pick_ssid, ssid, sizeof(g_wifi_pick_ssid) - 1);
  g_wifi_pick_ssid[sizeof(g_wifi_pick_ssid) - 1] = 0;
  g_wifi_edit_mode = WIFI_EDIT_PSK;
  switch_screen(SCR_WIFI_PSK);
}

static void on_wifi_pick_manual(lv_event_t *e) {
  // SSID manuell eingeben (für Fall dass Scan nicht klappt)
  g_wifi_pick_ssid[0] = 0;
  g_wifi_edit_mode = WIFI_EDIT_SSID;
  switch_screen(SCR_WIFI_PSK);
}

static void on_wifi_disconnect(lv_event_t *e) {
  wifi_clear_creds();
  switch_screen(SCR_WIFI);
}

static void on_wifi_open_ota(lv_event_t *e) {
  switch_screen(SCR_OTA);
}

void build_wifi(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 16, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 12, LV_PART_MAIN);

  // Header
  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, LV_PCT(100), 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_wifi_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← PROFIL");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);

  lv_obj_t *kicker = lv_label_create(scr);
  lv_label_set_text(kicker, "WLAN");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  // Status-Block
  lv_obj_t *status = lv_obj_create(scr);
  lv_obj_remove_style_all(status);
  lv_obj_set_size(status, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(status, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_top(status, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(status, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(status, 4, LV_PART_MAIN);
  lv_obj_set_style_border_side(status, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(status, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(status, 1, LV_PART_MAIN);

  lv_obj_t *t = lv_label_create(status);
  if (g_wifi.connected) {
    char b[80];
    snprintf(b, sizeof(b), "verbunden mit %s", g_wifi.ssid);
    lv_label_set_text(t, b);
    lv_obj_set_style_text_color(t, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  } else if (g_wifi.enabled) {
    char b[80];
    snprintf(b, sizeof(b), "verbinde mit %s …", g_wifi.ssid);
    lv_label_set_text(t, b);
    lv_obj_set_style_text_color(t, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  } else {
    lv_label_set_text(t, "nicht verbunden");
    lv_obj_set_style_text_color(t, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  }
  lv_obj_set_style_text_font(t, &moki_fraunces_italic_28, LV_PART_MAIN);
  lv_obj_set_width(t, LV_PCT(100));
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);

  if (g_wifi.connected && g_wifi.ip[0]) {
    lv_obj_t *ipl = lv_label_create(status);
    char b[40];
    snprintf(b, sizeof(b), "ip %s · rssi %d dBm", g_wifi.ip, WiFi.RSSI());
    lv_label_set_text(ipl, b);
    lv_obj_set_style_text_font(ipl, &moki_jetbrains_mono_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(ipl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  }

  if (g_wifi.enabled) {
    // Action-Reihe: TRENNEN | UPDATEN (wenn connected)
    lv_obj_t *actions = lv_obj_create(status);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), 50);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 12, LV_PART_MAIN);

    // TRENNEN
    lv_obj_t *dc = lv_obj_create(actions);
    lv_obj_remove_style_all(dc);
    lv_obj_set_flex_grow(dc, 1);
    lv_obj_set_height(dc, 50);
    lv_obj_set_style_bg_color(dc, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dc, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_border_width(dc, 1, LV_PART_MAIN);
    lv_obj_set_flex_flow(dc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dc, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(dc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dc, on_wifi_disconnect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(dc);
    lv_label_set_text(dl, "TRENNEN");
    lv_obj_set_style_text_font(dl, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(dl, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(dl, 3, LV_PART_MAIN);
    lv_obj_add_flag(dl, LV_OBJ_FLAG_EVENT_BUBBLE);

    // UPDATE — nur wenn aktiv connected (nicht nur enabled)
    if (g_wifi.connected) {
      lv_obj_t *up = lv_obj_create(actions);
      lv_obj_remove_style_all(up);
      lv_obj_set_flex_grow(up, 1);
      lv_obj_set_height(up, 50);
      lv_obj_set_style_bg_color(up, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(up, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_flex_flow(up, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(up, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_add_flag(up, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(up, on_wifi_open_ota, LV_EVENT_CLICKED, NULL);
      lv_obj_t *ul = lv_label_create(up);
      lv_label_set_text(ul, "UPDATE");
      lv_obj_set_style_text_font(ul, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(ul, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_text_letter_space(ul, 3, LV_PART_MAIN);
      lv_obj_add_flag(ul, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
  }

  // Liste der gefundenen Netzwerke (Sync-Scan, blockt ~3s)
  lv_obj_t *kicker2 = lv_label_create(scr);
  lv_label_set_text(kicker2, "VERFÜGBARE NETZE");
  lv_obj_set_style_text_font(kicker2, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker2, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker2, 3, LV_PART_MAIN);
  lv_obj_set_style_pad_top(kicker2, 8, LV_PART_MAIN);

  // WiFi.scanNetworks() — synchroner Scan. Robust:
  // 1. Mode auf STA setzen (falls aus WIFI_OFF kommend)
  // 2. Disconnect für sauberen State (sonst kann Scan failen wenn versucht
  //    wird aktiv zu connecten zu altem AP)
  // 3. ~250ms warten damit Radio bereit ist
  // 4. Scan mit explizit längeren Channel-Zeiten (default ist OK aber lieber
  //    sicher als 0 Ergebnisse)
  // Vorher SSID-Strings zwischenspeichern, sonst sind sie nach scanDelete weg.
  if (WiFi.getMode() != WIFI_STA) {
    Serial.println(F("[wifi] enabling STA for scan ..."));
    WiFi.mode(WIFI_STA);
    WiFi.onEvent(wifi_event_cb);
    delay(500);   // Radio cold-start: Calibration + RF-init braucht ~400ms
  }
  WiFi.disconnect(false, true);   // clear driver config, keep our NVS
  delay(200);
  Serial.println(F("[wifi] starting scan ..."));
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false,
                             /*passive=*/false, /*max_ms_per_chan=*/400);
  Serial.printf("[wifi] scan found %d networks\n", n);
  if (n < 0) n = 0;
  if (n > 12) n = 12;

  static char scan_ssids[12][33];
  static int  scan_rssi[12];
  static int  scan_count = 0;
  scan_count = n;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    strncpy(scan_ssids[i], s.c_str(), sizeof(scan_ssids[i]) - 1);
    scan_ssids[i][sizeof(scan_ssids[i]) - 1] = 0;
    scan_rssi[i] = WiFi.RSSI(i);
  }

  if (n == 0) {
    lv_obj_t *empty = lv_label_create(scr);
    lv_label_set_text(empty, "(keine Netze gefunden — manuell unten eingeben)");
    lv_obj_set_style_text_font(empty, &moki_fraunces_italic_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(empty, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  }

  for (int i = 0; i < scan_count; i++) {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_wifi_pick, LV_EVENT_CLICKED, scan_ssids[i]);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, scan_ssids[i]);
    lv_obj_set_style_text_font(l, &moki_fraunces_italic_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Signal-Stärke als Glyph
    int rssi = scan_rssi[i];
    const char *bars = (rssi > -55) ? "▮▮▮"
                     : (rssi > -70) ? "▮▮·"
                     : (rssi > -82) ? "▮··"
                                    : "···";
    lv_obj_t *r = lv_label_create(row);
    lv_label_set_text(r, bars);
    lv_obj_set_style_text_font(r, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
  }

  WiFi.scanDelete();

  // Manual-SSID-Eingabe als Fallback. Auch wenn Scan-Liste leer ist oder
  // Lucas's Netz versteckt ist (hidden SSID).
  lv_obj_t *manual = lv_obj_create(scr);
  lv_obj_remove_style_all(manual);
  lv_obj_set_size(manual, LV_PCT(100), 56);
  lv_obj_set_style_border_color(manual, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_border_width(manual, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(manual, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(manual, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(manual, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(manual, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(manual, on_wifi_pick_manual, LV_EVENT_CLICKED, NULL);
  lv_obj_t *ml = lv_label_create(manual);
  lv_label_set_text(ml, "+ NETZWERK MANUELL EINGEBEN");
  lv_obj_set_style_text_font(ml, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(ml, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(ml, 2, LV_PART_MAIN);
  lv_obj_add_flag(ml, LV_OBJ_FLAG_EVENT_BUBBLE);
}

void build_wifi_psk(void) {
  // Dual-Mode: WIFI_EDIT_SSID (manuell SSID eingeben) oder WIFI_EDIT_PSK (Passwort)
  if (g_wifi_edit_mode == WIFI_EDIT_PSK && !g_wifi_pick_ssid[0]) {
    switch_screen(SCR_WIFI); return;
  }

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

  // Header
  lv_obj_t *hdr = lv_obj_create(scr);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_size(hdr, LV_PCT(100), 56);
  lv_obj_set_style_pad_left(hdr, 24, LV_PART_MAIN);
  lv_obj_set_style_pad_right(hdr, 24, LV_PART_MAIN);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *back = lv_obj_create(hdr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, 100, 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_wifi_psk_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← ZURÜCK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_add_flag(bl, LV_OBJ_FLAG_EVENT_BUBBLE);

  char title[40];
  if (g_wifi_edit_mode == WIFI_EDIT_SSID) {
    snprintf(title, sizeof(title), "WLAN · netzwerkname");
  } else {
    snprintf(title, sizeof(title), "WLAN · %s", g_wifi_pick_ssid);
  }
  lv_obj_t *kicker = lv_label_create(hdr);
  lv_label_set_text(kicker, title);
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 2, LV_PART_MAIN);

  lv_obj_t *save = lv_obj_create(hdr);
  lv_obj_remove_style_all(save);
  lv_obj_set_size(save, 100, 36);
  lv_obj_set_style_bg_color(save, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(save, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flex_flow(save, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(save, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(save, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(save, on_wifi_psk_save, LV_EVENT_CLICKED, NULL);
  lv_obj_t *sl = lv_label_create(save);
  lv_label_set_text(sl, "OK");
  lv_obj_set_style_text_font(sl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sl, 3, LV_PART_MAIN);
  lv_obj_add_flag(sl, LV_OBJ_FLAG_EVENT_BUBBLE);

  // Textarea (PSK in plain — kein **** Maskieren erstmal)
  lv_obj_t *ta = lv_textarea_create(scr);
  lv_obj_set_size(ta, LV_PCT(100), 80);
  lv_obj_set_style_text_font(ta, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(ta, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_color(ta, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_border_width(ta, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ta, 16, LV_PART_MAIN);
  lv_textarea_set_one_line(ta, true);
  if (g_wifi_edit_mode == WIFI_EDIT_SSID) {
    lv_textarea_set_placeholder_text(ta, "ssid eingeben (z.B. MeinIPhone)");
  } else {
    lv_textarea_set_placeholder_text(ta, "passwort eingeben");
  }
  g_wifi_psk_ta = ta;

  // Keyboard
  lv_obj_t *kb = lv_keyboard_create(scr);
  lv_keyboard_set_textarea(kb, ta);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_size(kb, LV_PCT(100), 380);
  lv_obj_set_style_text_font(kb, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_add_event_cb(kb, on_wifi_psk_kb_event, LV_EVENT_ALL, NULL);
}
#else
// Stub-Builder wenn WiFi nicht im Build aktiv
void build_wifi(void)     { switch_screen(SCR_PROFILE); }
void build_wifi_psk(void) { switch_screen(SCR_PROFILE); }
#endif // MOKI_WIFI

void build_pin_rename(void) {
  if (g_active_pin < 0 || g_active_pin >= (int)g_pins_n) {
    switch_screen(SCR_MAP); return;
  }
  build_pin_edit_screen("ORT UMBENENNEN", g_pins[g_active_pin].name);
}
void build_pin_note(void) {
  if (g_active_pin < 0 || g_active_pin >= (int)g_pins_n) {
    switch_screen(SCR_MAP); return;
  }
  build_pin_edit_screen("NOTIZ", g_pins[g_active_pin].note);
}

void build_pin_detail(void) {
  if (g_active_pin < 0 || g_active_pin >= (int)g_pins_n) {
    switch_screen(SCR_MAP); return;
  }
  const moki_pin_t *p = &g_pins[g_active_pin];

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 32, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 32, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 18, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 18, LV_PART_MAIN);

  // Header: ← back
  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, LV_PCT(100), 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_pin_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← KARTE");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);

  // Kicker
  lv_obj_t *kicker = lv_label_create(scr);
  lv_label_set_text(kicker, "ORT");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  // Name
  lv_obj_t *name = lv_label_create(scr);
  lv_label_set_text(name, p->name);
  lv_obj_set_style_text_font(name, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(name, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_width(name, LV_PCT(100));
  lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);

  // Coords
  char coords[64];
  snprintf(coords, sizeof(coords), "%.4f °N · %.4f °O", p->lat, p->lon);
  lv_obj_t *cl = lv_label_create(scr);
  lv_label_set_text(cl, coords);
  lv_obj_set_style_text_font(cl, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(cl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);

  // Note (or hint if empty)
  lv_obj_t *note = lv_label_create(scr);
  if (p->note[0]) {
    lv_label_set_text(note, p->note);
    lv_obj_set_style_text_color(note, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  } else {
    lv_label_set_text(note, "(noch keine notiz · tippe »NOTIZ« unten)");
    lv_obj_set_style_text_color(note, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  }
  lv_obj_set_style_text_font(note, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, LV_PCT(100));
  lv_obj_set_flex_grow(note, 1);
  lv_obj_set_style_text_line_space(note, 6, LV_PART_MAIN);

  // Action-Reihe: UMBENENNEN | NOTIZ
  lv_obj_t *actions = lv_obj_create(scr);
  lv_obj_remove_style_all(actions);
  lv_obj_set_size(actions, LV_PCT(100), 50);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(actions, 12, LV_PART_MAIN);
  auto add_action = [&](const char *label, lv_event_cb_t cb) {
    lv_obj_t *b = lv_obj_create(actions);
    lv_obj_remove_style_all(b);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, 50);
    lv_obj_set_style_bg_color(b, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(l, 3, LV_PART_MAIN);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
  };
  add_action("UMBENENNEN", on_pin_open_rename);
  add_action("NOTIZ",      on_pin_open_note);
  add_action("TEILEN",     on_pin_share_all);

  // Delete button (groß, klar markiert)
  lv_obj_t *del = lv_obj_create(scr);
  lv_obj_remove_style_all(del);
  lv_obj_set_size(del, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(del, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(del, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(del, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(del, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(del, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(del, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(del, on_pin_delete, LV_EVENT_CLICKED, NULL);
  lv_obj_t *dl = lv_label_create(del);
  lv_label_set_text(dl, "ORT LÖSCHEN");
  lv_obj_set_style_text_font(dl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(dl, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(dl, 3, LV_PART_MAIN);
  lv_obj_add_flag(dl, LV_OBJ_FLAG_EVENT_BUBBLE);
}

void build_reader(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 36, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 36, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 14, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 14, LV_PART_MAIN);

  // Resolve title for header
  const char *book_title;
  if (g_book_active_idx == -2)      book_title = "anleitung";
  else if (g_book_active_idx == -1) book_title = "walden";
  else if (g_book_active_idx >= 0 && g_book_active_idx < g_book_count)
                                    book_title = g_books[g_book_active_idx].title;
  else                              book_title = "buch";

  // Header: "← title" — single tap-target, dezent
  lv_obj_t *header = lv_obj_create(scr);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, LV_PCT(100), 32);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(header, on_reader_back, LV_EVENT_CLICKED, NULL);

  char head[64];
  snprintf(head, sizeof(head), "← %s", book_title);
  lv_obj_t *hl = lv_label_create(header);
  lv_label_set_text(hl, head);
  lv_obj_set_style_text_font(hl, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(hl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(hl, 1, LV_PART_MAIN);
  lv_obj_add_flag(hl, LV_OBJ_FLAG_EVENT_BUBBLE);

  // Pagination
  size_t total = book_total_length();
  int total_pages = (int)((total + BOOK_CHARS_PER_PAGE - 1) / BOOK_CHARS_PER_PAGE);
  if (total_pages < 1) total_pages = 1;
  if (g_book_page >= total_pages) g_book_page = total_pages - 1;
  if (g_book_page < 0) g_book_page = 0;

  // Read current page
  static char page_buf[BOOK_CHARS_PER_PAGE + 8];
  size_t start = g_book_page * BOOK_CHARS_PER_PAGE;
  size_t want  = BOOK_CHARS_PER_PAGE;
  if (start + want > total) want = total - start;
  size_t got = book_read_chunk(start, want, page_buf, sizeof(page_buf));
  if (got == 0) {
    strncpy(page_buf, "(leer)", sizeof(page_buf));
    got = strlen(page_buf);
  }
  // Trim back to last whitespace to avoid mid-word cut
  if (g_book_page < total_pages - 1) {
    for (int i = (int)got - 1; i > (int)got - 80 && i > 0; i--) {
      if (page_buf[i] == ' ' || page_buf[i] == '\n') {
        page_buf[i] = 0;
        break;
      }
    }
  }

  // Body — füllt den ganzen Bildschirm zwischen Header und Footer
  lv_obj_t *body = lv_label_create(scr);
  lv_label_set_text(body, page_buf);
  lv_obj_set_style_text_font(body, &moki_fraunces_italic_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(body, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(body, LV_PCT(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_style_text_line_space(body, 7, LV_PART_MAIN);

  // Footer-Navi: ZURÜCK · Seite X/Y · WEITER
  lv_obj_t *nav = lv_obj_create(scr);
  lv_obj_remove_style_all(nav);
  lv_obj_set_size(nav, LV_PCT(100), 44);
  lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
  lv_obj_set_style_border_color(nav, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(nav, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_top(nav, 8, LV_PART_MAIN);

  lv_obj_t *prev = lv_label_create(nav);
  lv_label_set_text(prev, "← ZURÜCK");
  lv_obj_set_style_text_font(prev, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(prev,
      lv_color_hex(g_book_page > 0 ? MOKI_INK : MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(prev, 2, LV_PART_MAIN);
  if (g_book_page > 0) {
    lv_obj_add_flag(prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(prev, on_reader_prev, LV_EVENT_CLICKED, NULL);
  }

  char pagebuf[24];
  int pct = total_pages > 1 ? (g_book_page * 100) / (total_pages - 1) : 0;
  if (pct > 100) pct = 100;
  snprintf(pagebuf, sizeof(pagebuf), "%d / %d · %d%%", g_book_page + 1, total_pages, pct);
  lv_obj_t *page = lv_label_create(nav);
  lv_label_set_text(page, pagebuf);
  lv_obj_set_style_text_font(page, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(page, lv_color_hex(MOKI_DARK), LV_PART_MAIN);

  lv_obj_t *next = lv_label_create(nav);
  lv_label_set_text(next, "WEITER →");
  lv_obj_set_style_text_font(next, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(next,
      lv_color_hex(g_book_page < total_pages - 1 ? MOKI_INK : MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(next, 2, LV_PART_MAIN);
  if (g_book_page < total_pages - 1) {
    lv_obj_add_flag(next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(next, on_reader_next, LV_EVENT_CLICKED, NULL);
  }
}

void build_chat_detail(void) {
  // New-style targeting takes precedence: kind=0 (channel) or 1 (DM).
  // If neither set, fall back to legacy mockup-list (only for old code paths).
  bool new_style = (g_chat_target_kind >= 0);
  if (!new_style && (g_active_chat < 0 || g_active_chat >= SAMPLE_CHATS_COUNT)) {
    switch_screen(SCR_CHAT); return;
  }

  // Resolve display title + filter values for the new-style path.
  char title_buf[40] = "";
  int  filter_kind  = -1;   // -1=both/legacy, 0=channel, 1=dm
  int  filter_idx   = 0;
  char dm_contact_name[40] = "";
  if (new_style) {
    if (g_chat_target_kind == 0) {
      // MeshCore channel
      if (g_chat_target_idx < 0 || g_chat_target_idx >= g_num_channels) {
        switch_screen(SCR_CHAT); return;
      }
      // Switch active channel so outgoing TX targets this room.
      if (g_settings.mesh_active_channel != g_chat_target_idx) {
        g_settings.mesh_active_channel = (uint8_t)g_chat_target_idx;
        state_save_settings();
      }
      snprintf(title_buf, sizeof(title_buf), "#%s", g_channels[g_chat_target_idx].name);
      filter_kind = 0;
      filter_idx  = g_chat_target_idx;
    } else if (g_chat_target_kind == 1) {
      uint8_t key4[4];
      if (!moki_mesh_get_contact(g_chat_target_idx, dm_contact_name,
                                  sizeof(dm_contact_name), key4)) {
        switch_screen(SCR_CHAT); return;
      }
      strncpy(title_buf, dm_contact_name, sizeof(title_buf) - 1);
      title_buf[sizeof(title_buf) - 1] = 0;
      filter_kind = 1;
      // Set DM target for the compose-save handler.
      g_dm_target_idx = g_chat_target_idx;
    }
  }

  // Legacy mockup-chat compatibility (unused once dock-chat lands on SCR_CHAT).
  const moki_chat_t *c = NULL;
  bool is_lora = false;
  const chat_msg_t *msgs = NULL; int mn = 0;
  if (!new_style) {
    c = &SAMPLE_CHATS[g_active_chat];
    is_lora = !strcmp(c->kind, "lora");
    if (!is_lora) {
      if      (g_active_chat == 1) { msgs = MSGS_LINA;       mn = 2; }
      else if (g_active_chat == 2) { msgs = MSGS_LESEKREIS;  mn = 3; }
      else if (g_active_chat == 3) { msgs = MSGS_RHEIN;      mn = 3; }
    }
  } else {
    is_lora = true;   // new-style always shows the LoRa-style detail with bubbles
  }

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(scr, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scr, 16, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 12, LV_PART_MAIN);

  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, LV_PCT(100), 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_chat_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← ZURÜCK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);

  lv_obj_t *kicker = lv_label_create(scr);
  char k[48];
  if (new_style) {
    snprintf(k, sizeof(k), "%s",
             filter_kind == 1 ? "DIREKTNACHRICHT" : "MESH-KANAL");
  } else {
    snprintf(k, sizeof(k), "%s · %s",
             chat_kind_glyph(c->kind),
             !strcmp(c->kind,"direct") ? "DIREKT" :
             !strcmp(c->kind,"group")  ? "GRUPPE" : "ÖFFENTLICH");
  }
  lv_label_set_text(kicker, k);
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, new_style ? title_buf : c->name);
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  // Reset phrase only relevant in legacy mockup-chat path (where c != NULL).
  if (!new_style && c && c->reset && c->reset[0]) {
    lv_obj_t *r = lv_label_create(scr);
    lv_label_set_text(r, chat_reset_phrase(c->reset));
    lv_obj_set_style_text_font(r, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(r, 2, LV_PART_MAIN);
  }

  if (is_lora) {
    // ── Standalone cog when new-style (no channel chips needed) ──────────
    if (new_style) {
      lv_obj_t *cog_row = lv_obj_create(scr);
      lv_obj_remove_style_all(cog_row);
      lv_obj_set_size(cog_row, LV_PCT(100), 50);
      lv_obj_set_flex_flow(cog_row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(cog_row, LV_FLEX_ALIGN_END,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_t *cog = lv_obj_create(cog_row);
      lv_obj_remove_style_all(cog);
      lv_obj_set_size(cog, 60, 50);
      lv_obj_set_style_bg_color(cog,
          lv_color_hex(g_show_lora_advanced ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(cog, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(cog, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_border_width(cog, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(cog, 2, LV_PART_MAIN);
      lv_obj_set_flex_flow(cog, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(cog, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_add_flag(cog, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(cog, on_lora_advanced_toggle, LV_EVENT_CLICKED, NULL);
      lv_obj_t *cl = lv_label_create(cog);
      lv_label_set_text(cl, "···");
      lv_obj_set_style_text_font(cl, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(cl,
          lv_color_hex(g_show_lora_advanced ? MOKI_PAPER : MOKI_DARK), LV_PART_MAIN);
    }

    // ── Channel-Picker row — only legacy view (new_style entered via list) ──
    if (g_num_channels > 0 && !new_style) {
      lv_obj_t *row = lv_obj_create(scr);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, LV_PCT(100), 50);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
      lv_obj_t *ch_strip = lv_obj_create(row);
      lv_obj_remove_style_all(ch_strip);
      lv_obj_set_flex_grow(ch_strip, 1);
      lv_obj_set_height(ch_strip, 50);
      lv_obj_set_flex_flow(ch_strip, LV_FLEX_FLOW_ROW);
      lv_obj_set_style_pad_column(ch_strip, 6, LV_PART_MAIN);

      // Cog/advanced button on the right — shows/hides the preset picker.
      lv_obj_t *cog = lv_obj_create(row);
      lv_obj_remove_style_all(cog);
      lv_obj_set_size(cog, 50, 50);
      lv_obj_set_style_bg_color(cog,
          lv_color_hex(g_show_lora_advanced ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(cog, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(cog, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_border_width(cog, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(cog, 2, LV_PART_MAIN);
      lv_obj_set_flex_flow(cog, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(cog, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_add_flag(cog, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(cog, on_lora_advanced_toggle, LV_EVENT_CLICKED, NULL);
      lv_obj_t *cl = lv_label_create(cog);
      lv_label_set_text(cl, "···");   // three dots — "more options" idiom (font has no ⚙ glyph)
      lv_obj_set_style_text_font(cl, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(cl,
          lv_color_hex(g_show_lora_advanced ? MOKI_PAPER : MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_text_letter_space(cl, 1, LV_PART_MAIN);
      for (int ci = 0; ci < g_num_channels; ci++) {
        bool active = (ci == g_settings.mesh_active_channel);
        lv_obj_t *c = lv_obj_create(ch_strip);
        lv_obj_remove_style_all(c);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_height(c, 50);
        lv_obj_set_style_bg_color(c,
            lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(c, lv_color_hex(MOKI_INK), LV_PART_MAIN);
        lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(c, 2, LV_PART_MAIN);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, on_mesh_channel_picked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)ci);
        lv_obj_t *l = lv_label_create(c);
        char tag[24];
        snprintf(tag, sizeof(tag), "#%s", g_channels[ci].name);
        lv_label_set_text(l, tag);
        lv_obj_set_style_text_font(l, &moki_jetbrains_mono_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(l,
            lv_color_hex(active ? MOKI_PAPER : MOKI_DARK), LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
      }
    }

    // ── Preset-Picker — only when advanced toggle is on ──────────────────
    if (g_show_lora_advanced) {
    lv_obj_t *preset_strip = lv_obj_create(scr);
    lv_obj_remove_style_all(preset_strip);
    lv_obj_set_size(preset_strip, LV_PCT(100), 50);
    lv_obj_set_flex_flow(preset_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(preset_strip, 6, LV_PART_MAIN);
    for (uint8_t pi = 0; pi < LORA_PRESET_COUNT; pi++) {
      bool active = (g_settings.lora_preset == pi);
      lv_obj_t *c = lv_obj_create(preset_strip);
      lv_obj_remove_style_all(c);
      lv_obj_set_flex_grow(c, 1);
      lv_obj_set_height(c, 50);
      lv_obj_set_style_bg_color(c,
          lv_color_hex(active ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(c, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(c, 2, LV_PART_MAIN);
      lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(c, on_lora_preset_picked, LV_EVENT_CLICKED,
                          (void *)(intptr_t)pi);
      lv_obj_t *l = lv_label_create(c);
      lv_label_set_text(l, LORA_PRESET_LABELS[pi]);
      lv_obj_set_style_text_font(l, &moki_jetbrains_mono_18, LV_PART_MAIN);
      lv_obj_set_style_text_color(l,
          lv_color_hex(active ? MOKI_PAPER : MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
    }
    }   // end if (g_show_lora_advanced)

    // ── Big stat tile: RX-Count + last RSSI/SNR + secs since last RX ─────
    lv_obj_t *stat = lv_obj_create(scr);
    lv_obj_remove_style_all(stat);
    lv_obj_set_size(stat, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(stat, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stat, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(stat, lv_color_hex(MOKI_MID), LV_PART_MAIN);
    lv_obj_set_style_border_width(stat, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(stat, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stat, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(stat, LV_FLEX_FLOW_COLUMN);

    // Big RX counter
    char big[32];
    snprintf(big, sizeof(big), "%lu pakete", (unsigned long)g_lora_rx_count);
    lv_obj_t *bigl = lv_label_create(stat);
    lv_label_set_text(bigl, big);
    lv_obj_set_style_text_font(bigl, &moki_fraunces_italic_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(bigl, lv_color_hex(MOKI_INK), LV_PART_MAIN);

    // RSSI + SNR + seconds since last
    char detail[96];
    int64_t age_diff = (int64_t)millis() - (int64_t)g_lora_last_rx_ms;
    if (g_lora_last_rx_ms > 0 && age_diff >= 0) {
      uint32_t since = (uint32_t)(age_diff / 1000);
      snprintf(detail, sizeof(detail),
               "letztes signal: %d dBm · snr %.1f · vor %lus",
               g_lora_last_rssi, g_lora_last_snr, (unsigned long)since);
    } else if (g_lora_msg_count > 0) {
      snprintf(detail, sizeof(detail),
               "%d nachrichten gespeichert · diese sitzung still",
               g_lora_msg_count);
    } else {
      snprintf(detail, sizeof(detail), "noch kein signal empfangen");
    }
    lv_obj_t *det = lv_label_create(stat);
    lv_label_set_text(det, detail);
    lv_obj_set_style_text_font(det, &moki_jetbrains_mono_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(det, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(det, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_top(det, 6, LV_PART_MAIN);

    // Compact tech-line: TX-state + status
    char status[80];
    snprintf(status, sizeof(status), "%s · TX %lu · %s",
             g_lora_status,
             (unsigned long)g_lora_tx_count,
             g_settings.lora_tx_armed ? "TX FREI" : "RX-ONLY");
    lv_obj_t *st = lv_label_create(scr);
    lv_label_set_text(st, status);
    lv_obj_set_style_text_font(st, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(st, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(st, 1, LV_PART_MAIN);

    if (g_lora_msg_count == 0) {
      lv_obj_t *empty = lv_label_create(scr);
      lv_label_set_text(empty,
        "noch keine nachrichten empfangen.\n"
        "antenne dran, dann signale aus dem äther.");
      lv_obj_set_style_text_font(empty, &moki_fraunces_italic_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(empty, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_width(empty, LV_PCT(100));
      lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    } else {
      // Filter messages by the chat target chosen via the chats-list row.
      //   new_style channel: show only msgs with channel_idx == filter_idx
      //   new_style DM     : show only msgs with channel_idx == -1 AND
      //                      msg.from prefix matches the contact name
      //   legacy           : all messages on the active channel
      // Foreign-protocol (channel_idx=-2) only when advanced toggle is on.
      int legacy_filter_idx = (int)g_settings.mesh_active_channel;
      int start = (g_lora_msg_count == LORA_MSG_CAP) ? g_lora_msg_head : 0;
      int rendered = 0;
      for (int n = 0; n < g_lora_msg_count; n++) {
        int idx = (start + n) % LORA_MSG_CAP;
        const moki_lora_msg_t *m = &g_lora_msgs[idx];

        // Foreign-protocol bucket — only when advanced toggle is on.
        if (m->channel_idx == -2 && !g_show_lora_advanced) continue;

        if (new_style) {
          if (filter_kind == 0) {
            // Channel: only matching channel_idx (DMs visible too if directed
            // here? No — only msgs from this channel).
            if (m->channel_idx != filter_idx) continue;
          } else if (filter_kind == 1) {
            // DM: only DMs (channel_idx=-1) — bidirectional thread.
            //   incoming: m->from == dm_contact_name
            //   outgoing: m->from == "→ <dm_contact_name>" (compose handler tag)
            if (m->channel_idx != -1) continue;
            bool is_incoming = (strncmp(m->from, dm_contact_name, sizeof(m->from)) == 0);
            char out_tag[40];
            snprintf(out_tag, sizeof(out_tag), "→ %s", dm_contact_name);
            bool is_outgoing = (strncmp(m->from, out_tag, sizeof(m->from)) == 0);
            if (!is_incoming && !is_outgoing) continue;
          }
        } else {
          // Legacy path: same as before
          if (m->channel_idx >= 0 && m->channel_idx != legacy_filter_idx) continue;
        }
        rendered++;

        lv_obj_t *bubble = lv_obj_create(scr);
        lv_obj_remove_style_all(bubble);
        lv_obj_set_size(bubble, LV_PCT(95), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(bubble, lv_color_hex(MOKI_MID), LV_PART_MAIN);
        lv_obj_set_style_border_width(bubble, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(bubble, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bubble, 12, LV_PART_MAIN);
        lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

        // Header: from · time · (RSSI/SNR · preset only when advanced toggled)
        char hdr[120];
        char tstr[16];
        format_clock(m->ts_ms, tstr, sizeof(tstr));
        if (g_show_lora_advanced) {
          const char *plabel = (m->preset < LORA_PRESET_COUNT
                                ? LORA_PRESET_LABELS[m->preset] : "?");
          if (m->rssi)
            snprintf(hdr, sizeof(hdr), "%s · %s · %d dBm · %s",
                     m->from, tstr, m->rssi, plabel);
          else
            snprintf(hdr, sizeof(hdr), "%s · %s · %s", m->from, tstr, plabel);
        } else {
          // Default: clean WhatsApp-style header — name and time only.
          snprintf(hdr, sizeof(hdr), "%s · %s", m->from, tstr);
        }
        lv_obj_t *h = lv_label_create(bubble);
        lv_label_set_text(h, hdr);
        lv_obj_set_style_text_font(h, &moki_jetbrains_mono_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(h, 1, LV_PART_MAIN);

        // Body: NUL-terminated copy with non-printables replaced by '.'
        char safe[sizeof(m->text) + 1];
        size_t tn = m->len;
        if (tn > sizeof(m->text)) tn = sizeof(m->text);
        for (size_t i = 0; i < tn; i++) {
          uint8_t b = (uint8_t)m->text[i];
          safe[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        safe[tn] = 0;
        lv_obj_t *t = lv_label_create(bubble);
        lv_label_set_text(t, safe);
        lv_obj_set_style_text_font(t, &moki_jetbrains_mono_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(t, lv_color_hex(MOKI_INK), LV_PART_MAIN);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_top(t, 4, LV_PART_MAIN);
      }
    }

    lv_obj_t *send = lv_obj_create(scr);
    lv_obj_remove_style_all(send);
    lv_obj_set_size(send, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(send,
        lv_color_hex(g_settings.lora_tx_armed ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(send, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(send, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_border_width(send, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(send, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(send, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(send, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(send, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(send, on_lora_send_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(send);
    lv_label_set_text(sl, g_settings.lora_tx_armed ? "+ NACHRICHT SENDEN" : "ANTENNE DRAN? IN EINSTELLUNGEN ARMEN");
    lv_obj_set_style_text_font(sl, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(sl,
        lv_color_hex(g_settings.lora_tx_armed ? MOKI_PAPER : MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(sl, 2, LV_PART_MAIN);
  } else {
    // Sample-data conversation
    for (int i = 0; i < mn; i++) {
      lv_obj_t *bubble = lv_obj_create(scr);
      lv_obj_remove_style_all(bubble);
      lv_obj_set_size(bubble, LV_PCT(95), LV_SIZE_CONTENT);
      lv_obj_set_style_bg_color(bubble, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(bubble, lv_color_hex(MOKI_MID), LV_PART_MAIN);
      lv_obj_set_style_border_width(bubble, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(bubble, 2, LV_PART_MAIN);
      lv_obj_set_style_pad_all(bubble, 12, LV_PART_MAIN);
      lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

      char hdr[64]; snprintf(hdr, sizeof(hdr), "%s · %s", msgs[i].from, msgs[i].ts);
      lv_obj_t *h = lv_label_create(bubble);
      lv_label_set_text(h, hdr);
      lv_obj_set_style_text_font(h, &moki_jetbrains_mono_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(h, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_text_letter_space(h, 1, LV_PART_MAIN);

      lv_obj_t *t = lv_label_create(bubble);
      lv_label_set_text(t, msgs[i].text);
      lv_obj_set_style_text_font(t, &moki_fraunces_regular_36, LV_PART_MAIN);
      lv_obj_set_style_text_color(t, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_set_width(t, LV_PCT(100));
      lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_pad_top(t, 4, LV_PART_MAIN);
    }

    lv_obj_t *foot = lv_label_create(scr);
    lv_label_set_text(foot, "ANTWORTEN KOMMT IM NÄCHSTEN UPDATE.");
    lv_obj_set_style_text_font(foot, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(foot, lv_color_hex(MOKI_MID), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(foot, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_top(foot, 12, LV_PART_MAIN);
  }
}

// ----------------------------------------------------------------------------
// Forward declaration — actual lora_send lives in the LoRa section below.
static bool lora_send(const char *text);

// ============================================================================
// When >= 0, LoRa-Compose sends a DIRECT message to that contact index
// instead of broadcasting to the active channel. Reset to -1 after send.
// Non-static so the chats-list contact-tap handler can set it from above.
int g_dm_target_idx = -1;

// LoRa-Compose — small overlay reusing the German keyboard for chat input.
// ============================================================================
static char       g_lora_compose[160] = "";
static lv_obj_t  *g_lora_overlay      = NULL;
static lv_obj_t  *g_lora_compose_lbl  = NULL;

static void lora_compose_close(void) {
  if (g_lora_overlay) { lv_obj_del(g_lora_overlay); g_lora_overlay = NULL; g_lora_compose_lbl = NULL; }
}
static void on_lora_compose_cancel(lv_event_t *e) {
  g_lora_compose[0] = 0;
  lora_compose_close();
}
static void on_lora_compose_save(lv_event_t *e) {
  if (g_lora_compose[0] == 0) { lora_compose_close(); return; }
  bool ok = false;
  bool was_dm = (g_dm_target_idx >= 0);
  if (g_settings.lora_tx_armed) {
    if (was_dm) {
      // Direct-Message to a specific contact (M4 Phase 2).
      ok = moki_mesh_dm(g_dm_target_idx, g_lora_compose);
      if (ok) {
        char contact_name[32]; uint8_t key4[4];
        moki_mesh_get_contact(g_dm_target_idx, contact_name, sizeof(contact_name), key4);
        // Push to ring buffer with channel_idx=-1 so it lives in the DM
        // thread bucket, AND prefix the from-field with "→ <contact>" so
        // the DM-thread filter can match outgoing messages too.
        char outgoing_tag[40];
        snprintf(outgoing_tag, sizeof(outgoing_tag), "→ %s", contact_name);
        moki_lora_push_msg_channel(outgoing_tag, g_lora_compose, 0, -1);
      }
    } else {
      // Group-channel broadcast.
      ok = moki_mesh_send(g_settings.handle, g_lora_compose);
      if (ok) lora_push_msg(g_settings.handle, g_lora_compose, 0);
    }
  }
  show_toast(ok ? (was_dm ? "DM GESENDET" : "GESENDET ÜBER MESH")
                : "TX NICHT FREI · ANTENNE PRÜFEN");
  g_lora_compose[0] = 0;
  g_dm_target_idx = -1;   // reset DM mode regardless of result
  lora_compose_close();
  switch_screen(SCR_CHAT_DETAIL);
}
static void lora_compose_key_event(const char *key) {
  size_t len = strlen(g_lora_compose);
  if (!strcmp(key, "BACK")) {
    if (len > 0) {
      do { len--; } while (len > 0 && (((unsigned char)g_lora_compose[len]) & 0xC0) == 0x80);
      g_lora_compose[len] = 0;
    }
  } else if (!strcmp(key, "SPACE")) {
    if (len + 1 < sizeof(g_lora_compose)) { g_lora_compose[len] = ' '; g_lora_compose[len+1] = 0; }
  } else {
    size_t klen = strlen(key);
    if (len + klen + 1 < sizeof(g_lora_compose)) {
      memcpy(g_lora_compose + len, key, klen);
      g_lora_compose[len + klen] = 0;
    }
  }
  if (g_lora_compose_lbl)
    lv_label_set_text(g_lora_compose_lbl, g_lora_compose[0] ? g_lora_compose : "…");
}
static void on_lora_key_clicked(lv_event_t *e) {
  lora_compose_key_event((const char *)lv_event_get_user_data(e));
}

void open_lora_compose(void) {
  if (g_lora_overlay) return;
  g_lora_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(g_lora_overlay);
  lv_obj_set_size(g_lora_overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_lora_overlay, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_lora_overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flex_flow(g_lora_overlay, LV_FLEX_FLOW_COLUMN);

  // Header
  lv_obj_t *hdr = lv_obj_create(g_lora_overlay);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_size(hdr, LV_PCT(100), 56);
  lv_obj_set_style_pad_left(hdr, 24, LV_PART_MAIN);
  lv_obj_set_style_pad_right(hdr, 24, LV_PART_MAIN);
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(hdr, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *cancel = lv_label_create(hdr);
  lv_label_set_text(cancel, "ABBRECHEN");
  lv_obj_set_style_text_font(cancel, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(cancel, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(cancel, 2, LV_PART_MAIN);
  lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(cancel, on_lora_compose_cancel, LV_EVENT_CLICKED, NULL);

  lv_obj_t *t = lv_label_create(hdr);
  lv_label_set_text(t, "LORA SENDEN");
  lv_obj_set_style_text_font(t, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(t, 3, LV_PART_MAIN);

  lv_obj_t *send = lv_label_create(hdr);
  lv_label_set_text(send, "SENDEN");
  lv_obj_set_style_text_font(send, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(send, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(send, 2, LV_PART_MAIN);
  lv_obj_add_flag(send, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(send, on_lora_compose_save, LV_EVENT_CLICKED, NULL);

  // Body
  lv_obj_t *body = lv_obj_create(g_lora_overlay);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_style_pad_left(body, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_right(body, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_top(body, 24, LV_PART_MAIN);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 12, LV_PART_MAIN);

  lv_obj_t *kicker = lv_label_create(body);
  lv_label_set_text(kicker, "AN MOKI-MESH (868 MHZ)");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  g_lora_compose_lbl = lv_label_create(body);
  lv_label_set_text(g_lora_compose_lbl, g_lora_compose[0] ? g_lora_compose : "…");
  lv_obj_set_style_text_font(g_lora_compose_lbl, &moki_fraunces_regular_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(g_lora_compose_lbl, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_side(g_lora_compose_lbl, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_lora_compose_lbl, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_lora_compose_lbl, 2, LV_PART_MAIN);
  lv_obj_set_width(g_lora_compose_lbl, LV_PCT(100));
  lv_label_set_long_mode(g_lora_compose_lbl, LV_LABEL_LONG_WRAP);

  // Compact keyboard
  lv_obj_t *kb = lv_obj_create(g_lora_overlay);
  lv_obj_remove_style_all(kb);
  lv_obj_set_size(kb, LV_PCT(100), 240);
  lv_obj_set_style_bg_color(kb, lv_color_hex(MOKI_LIGHT), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
  lv_obj_set_style_border_color(kb, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(kb, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_top(kb, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(kb, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(kb, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(kb, 4, LV_PART_MAIN);

  static const char *r1[] = {"q","w","e","r","t","z","u","i","o","p"};
  static const char *r2[] = {"a","s","d","f","g","h","j","k","l","ä"};
  static const char *r3[] = {"y","x","c","v","b","n","m","ö","ü","ß"};
  auto build_kb_row = [&](const char *const *keys) {
    lv_obj_t *row = lv_obj_create(kb);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
    for (int i = 0; i < 10; i++) {
      lv_obj_t *btn = lv_obj_create(row);
      lv_obj_remove_style_all(btn);
      lv_obj_set_size(btn, 48, 50);
      lv_obj_set_style_bg_color(btn, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(btn, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(btn, 2, LV_PART_MAIN);
      lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(btn, on_lora_key_clicked, LV_EVENT_CLICKED, (void *)keys[i]);
      lv_obj_t *l = lv_label_create(btn);
      lv_label_set_text(l, keys[i]);
      lv_obj_set_style_text_font(l, &moki_fraunces_regular_36, LV_PART_MAIN);
      lv_obj_set_style_text_color(l, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_center(l);
    }
  };
  build_kb_row(r1); build_kb_row(r2); build_kb_row(r3);

  lv_obj_t *r4 = lv_obj_create(kb);
  lv_obj_remove_style_all(r4);
  lv_obj_set_size(r4, LV_PCT(100), 56);
  lv_obj_set_flex_flow(r4, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r4, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r4, 6, LV_PART_MAIN);
  auto big = [&](const char *txt, const char *kk, int w) {
    lv_obj_t *btn = lv_obj_create(r4);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, 50);
    lv_obj_set_style_bg_color(btn, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 2, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_lora_key_clicked, LV_EVENT_CLICKED, (void *)kk);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_center(l);
  };
  big("ENTF",        "BACK",  100);
  big("LEERZEICHEN", "SPACE", 280);
  big(".",           ".",     60);
}

// ============================================================================
// LoRa SX1262 — RX-by-default for safety. TX only when antenna confirmed via
// settings (lora_tx_armed). Power supply for radio + GPS is gated by IO0 of
// the XL9555 IO expander @ I2C 0x20.
// ============================================================================
ICACHE_RAM_ATTR static void lora_irq_cb(void) { g_lora_rx_flag = true; }

static void lora_power_on(void) {
  if (!g_xl9555.init(Wire, 39, 40, 0x20)) {
    Serial.println(F("[lora] XL9555 IO expander not found — LoRa offline"));
    g_lora_status = "NO IO";
    return;
  }
  g_xl9555.pinMode(ExtensionIOXL9555::IO0, OUTPUT);
  g_xl9555.digitalWrite(ExtensionIOXL9555::IO0, HIGH);
  delay(1500);
}

static void lora_init(void) {
  // Both LoRa and SD share SPI; ensure both CS pins are high before init.
  pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
  pinMode(SD_CS,   OUTPUT); digitalWrite(SD_CS,   HIGH);

  lora_power_on();
  SPI.begin(LORA_SCLK, LORA_MISO, LORA_MOSI);

  Serial.print(F("[lora] init... "));
  int state = g_radio.begin();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("FAILED code=%d\n", state);
    g_lora_status = "FAIL";
    return;
  }

  // EU 868 MHz, BW 250 kHz, SF 10, CR 6, sync 0xAB, preamble 15
  g_radio.setFrequency(868.0);
  g_radio.setBandwidth(250.0);
  g_radio.setSpreadingFactor(10);
  g_radio.setCodingRate(6);
  g_radio.setSyncWord(0xAB);
  g_radio.setPreambleLength(15);
  g_radio.setCRC(true);
  g_radio.setTCXO(2.4);
  g_radio.setDio2AsRfSwitch(true);
  // SAFE default: 0 dBm (1 mW). Even without antenna, brief TX is harmless.
  // When user arms via settings + has antenna, we step up to 14 dBm.
  g_radio.setOutputPower(0);
  g_radio.setCurrentLimit(60);

  // RX with interrupt
  g_radio.setPacketReceivedAction(lora_irq_cb);
  state = g_radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("startReceive failed code=%d\n", state);
    g_lora_status = "RX FAIL";
    return;
  }

  g_lora_ready  = true;
  g_lora_status = "RX";
  Serial.println(F("ok @ 868MHz, RX listening"));
}

// Parse "MOKI|<from>|<text>" — split by '|', limit 2 fields.
// Loose: if no header, treat the whole thing as anonymous text.
static void lora_handle_packet(const String &buf, int16_t rssi, float snr) {
  // Always dump raw hex first — useful for promiscuous scanning of foreign nets
  Serial.printf("[lora] rx %u bytes rssi=%d snr=%.1f raw=", (unsigned)buf.length(), rssi, snr);
  for (size_t i = 0; i < buf.length() && i < 96; i++) {
    Serial.printf("%02x", (uint8_t)buf[i]);
  }
  Serial.print(" ascii=\"");
  for (size_t i = 0; i < buf.length() && i < 96; i++) {
    char c = buf[i];
    Serial.print((c >= 0x20 && c < 0x7F) ? c : '.');
  }
  Serial.println('"');

  String from = "anon";
  size_t header_len = 0;
  if (buf.startsWith("MOKI|")) {
    int p1 = buf.indexOf('|', 5);
    if (p1 > 5) {
      from = buf.substring(5, p1);
      header_len = p1 + 1;
    }
  }
  // Push the RAW payload (post-header if MOKI|, else full packet) so we can
  // dump exact hex later. Foreign packets (Meshtastic/MeshCore) keep all bytes.
  const char *body = buf.c_str() + header_len;
  size_t body_len  = buf.length() - header_len;
  lora_push_msg_raw(from.c_str(), (const uint8_t *)body, body_len, rssi, snr);
  g_lora_rx_count++;
}

static void lora_poll(void) {
  if (!g_lora_ready) return;
  if (!g_lora_rx_flag) return;
  g_lora_rx_flag = false;

  String received;
  int state = g_radio.readData(received);
  int16_t rssi = (int16_t)g_radio.getRSSI();
  float snr = g_radio.getSNR();
  if (state == RADIOLIB_ERR_NONE && received.length() > 0) {
    g_lora_last_rssi  = rssi;
    g_lora_last_snr   = snr;
    g_lora_last_rx_ms = millis();
    lora_handle_packet(received, rssi, snr);
  } else {
    Serial.printf("[lora] rx_err code=%d rssi=%d snr=%.1f\n", state, rssi, snr);
  }
  g_radio.startReceive();
}

// Apply one of the LORA_PRESET_* configurations.
// Persistable — caller may save settings afterwards.
static void lora_reconfigure(float freq_mhz, float bw_khz, int sf, int cr, uint8_t sync, bool crc);

static void lora_apply_preset(uint8_t preset) {
  switch (preset) {
    case LORA_PRESET_MESHCORE_NARROW:
      lora_reconfigure(869.618, 62.5,  8, 8, 0x12, true);
      break;
    case LORA_PRESET_MESHCORE_LEGACY:
      lora_reconfigure(869.525, 250.0, 11, 5, 0x12, true);
      break;
    case LORA_PRESET_MESHTASTIC_LF:
      lora_reconfigure(869.525, 250.0, 11, 5, 0x2B, true);
      break;
    case LORA_PRESET_MOKI:
    default:
      lora_reconfigure(868.0,   250.0, 10, 6, 0xAB, true);
      break;
  }
}

// Reconfigure SX1262 on-the-fly — useful for scanning foreign networks.
// Always enables RX-boosted-gain for max sensitivity (~3dB better RX).
static void lora_reconfigure(float freq_mhz, float bw_khz, int sf, int cr, uint8_t sync, bool crc) {
  g_radio.standby();
  g_radio.setFrequency(freq_mhz);
  g_radio.setBandwidth(bw_khz);
  g_radio.setSpreadingFactor(sf);
  g_radio.setCodingRate(cr);
  g_radio.setSyncWord(sync);
  g_radio.setCRC(crc);
  g_radio.setRxBoostedGainMode(true);
  g_radio.setPacketReceivedAction(lora_irq_cb);
  g_radio.startReceive();
  Serial.printf("[lora] reconf %.3fMHz bw=%.1fkHz sf=%d cr=4/%d sync=0x%02X crc=%d boost=on\n",
                freq_mhz, bw_khz, sf, cr, sync, crc ? 1 : 0);
}

// Send a message via LoRa. Gated by settings: returns false if not armed.
static bool lora_send(const char *text) {
  if (!g_lora_ready) return false;
  if (!g_settings.lora_tx_armed) {
    Serial.println(F("[lora] tx not armed (no antenna confirmed)"));
    return false;
  }
  char pkt[200];
  snprintf(pkt, sizeof(pkt), "MOKI|%s|%s", g_settings.handle, text);
  Serial.printf("[lora] tx '%s'\n", pkt);
  // Bump output power for armed transmit (still moderate at 14 dBm = 25 mW)
  g_radio.setOutputPower(14);
  int state = g_radio.transmit((uint8_t *)pkt, strlen(pkt));
  g_radio.setOutputPower(0);   // back to safe-default
  g_radio.startReceive();      // re-arm RX
  if (state == RADIOLIB_ERR_NONE) {
    g_lora_tx_count++;
    // Echo locally so user sees their own message
    lora_push_msg(g_settings.handle, text, 0);
    return true;
  }
  Serial.printf("[lora] tx failed code=%d\n", state);
  return false;
}

// ============================================================================
// M2 — Real Time Clock (PCF85063 over I2C)
// ============================================================================
// The RTC is at I2C address 0x51. Wire is already initialised (touch + XL9555
// share the bus). We read time at boot, fall back to compile-time if the chip
// has never been set, and let serial commands update it.
//
// KNOWN ISSUE 2026-04-30: setDateTime() round-trip is broken in our hardware
// setup — values get corrupted on write (e.g. day 30→14, hour 12→04). Boot
// read is consistent (so passive use works), but set_time doesn't take effect.
// Suspected: chip-state / I2C-bus-contention with GT911+XL9555 sharing Wire.
// TODO: instrument with raw Wire transactions (probe CTRL1, CTRL2 status
// bits) to confirm whether writes land at all. Workaround for now: status
// bar shows whatever the RTC reads, which is at least monotonic.

static void rtc_init(void) {
  // SensorPCF85063::begin(Wire, addr) returns true on success.
  if (!g_rtc.begin(Wire, PCF85063_SLAVE_ADDRESS, 39, 40)) {
    Serial.println(F("[rtc] PCF85063 not found — clock disabled"));
    return;
  }
  g_rtc_ready = true;

  // Force the oscillator START + 24-hour mode by writing CTRL1 register
  // explicitly. Some PCF85063 boot states leave STOP=1 which freezes the
  // clock, and the SensorLib doesn't touch CTRL1. We want CAP_SEL=0 (12.5pF),
  // STOP=0, 12_24=0 (24-hour), CIE=0.
  Wire.beginTransmission(PCF85063_SLAVE_ADDRESS);
  Wire.write(0x00);   // CTRL1 register
  Wire.write(0x00);   // all flags clear → oscillator running, 24h, no IRQs
  Wire.endTransmission();

  RTC_DateTime now = g_rtc.getDateTime();
  Serial.printf("[rtc] boot time: %04u-%02u-%02u %02u:%02u:%02u\n",
                now.year, now.month, now.day, now.hour, now.minute, now.second);

  // Sanity-check: if the year is clearly bogus (chip never configured), seed
  // it with our compile-time so timestamps aren't from 1970.
  if (now.year < 2024 || now.year > 2099) {
    RTC_DateTime seed(__DATE__, __TIME__);   // parses "Apr 30 2026" / "12:34:56"
    g_rtc.setDateTime(seed);
    Serial.printf("[rtc] seeded from build: %04u-%02u-%02u %02u:%02u:%02u\n",
                  seed.year, seed.month, seed.day,
                  seed.hour, seed.minute, seed.second);
  }
}

// Days-since-Jan-1 helper (1..366), Zeller-ish but plain.
static uint16_t day_of_year(uint16_t y, uint8_t m, uint8_t d) {
  static const uint16_t cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  uint16_t doy = cum[m - 1] + d;
  if (leap && m > 2) doy += 1;
  return doy;
}

// Direct Wire write bypassing SensorLib (whose setDateTime is broken on our
// hardware — see Bug Tracker). One register at a time — block-writes were
// silently dropping HOUR (0x06) and DAY (0x07) on this hardware (likely a
// shared-bus contention with GT911/XL9555 — repeated single transactions
// land reliably).
static bool rtc_set_direct(uint16_t year, uint8_t month, uint8_t day,
                           uint8_t hour, uint8_t minute, uint8_t second) {
  auto dec2bcd = [](uint8_t v) -> uint8_t {
    return ((v / 10) << 4) | (v % 10);
  };
  auto write_reg = [](uint8_t reg, uint8_t val) -> bool {
    Wire.beginTransmission(PCF85063_SLAVE_ADDRESS);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
  };
  bool ok = true;
  ok &= write_reg(0x04, dec2bcd(second) & 0x7F);
  ok &= write_reg(0x05, dec2bcd(minute));
  ok &= write_reg(0x06, dec2bcd(hour));
  ok &= write_reg(0x07, dec2bcd(day));
  ok &= write_reg(0x08, 0);                       // weekday placeholder
  ok &= write_reg(0x09, dec2bcd(month));
  ok &= write_reg(0x0A, dec2bcd((uint8_t)(year % 100)));
  if (!ok) Serial.println(F("[rtc] one or more register writes failed"));
  return ok;
}

// Parse "YYYY-MM-DD HH:MM:SS" — returns true on success, fills out.
static bool rtc_parse(const String &s, RTC_DateTime &out) {
  // 19 chars expected: YYYY-MM-DD HH:MM:SS
  if (s.length() < 19) return false;
  out.year   = s.substring(0, 4).toInt();
  out.month  = s.substring(5, 7).toInt();
  out.day    = s.substring(8, 10).toInt();
  out.hour   = s.substring(11, 13).toInt();
  out.minute = s.substring(14, 16).toInt();
  out.second = s.substring(17, 19).toInt();
  if (out.year < 2024 || out.year > 2099) return false;
  if (out.month < 1 || out.month > 12) return false;
  if (out.day < 1 || out.day > 31) return false;
  if (out.hour > 23 || out.minute > 59 || out.second > 59) return false;
  return true;
}

// Periodic check — call from loop(). Runs habit-rollover when the date crosses.
static void rtc_tick(void) {
  if (!g_rtc_ready) return;
  static uint32_t last_check_ms = 0;
  uint32_t now_ms = millis();
  if (now_ms - last_check_ms < 30000) return;   // every 30s is plenty
  last_check_ms = now_ms;

  RTC_DateTime now = g_rtc.getDateTime();
  uint16_t doy = day_of_year(now.year, now.month, now.day);

  // First tick after boot: just record current day, don't roll yet.
  if (g_last_rollover_doy == 0) {
    g_last_rollover_doy = doy;
    return;
  }
  if (doy != g_last_rollover_doy) {
    // Day changed — shift habit history left by one slot per habit.
    Serial.printf("[rtc] midnight rollover %u -> %u\n",
                  g_last_rollover_doy, doy);
    for (int h = 0; h < g_habits_count; h++) {
      moki_habit_t *hb = &g_habits[h];
      // Slide: history[0] is oldest, history[83] is today.
      memmove(hb->history, hb->history + 1, 83);
      hb->history[83]   = 0;
      // today_count rolls into the just-completed day; reset for the new day.
      // (Done in-place by the slide above — yesterday became history[82].)
      hb->today_count   = 0;
    }
    state_save_habits();
    g_last_rollover_doy = doy;
  }
}

// ----------------------------------------------------------------------------
// ui_entry — initial render: home screen.
// ----------------------------------------------------------------------------
static void ui_entry(void) {
  switch_screen(SCR_HOME);
}

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setRxBufferSize(1024);   // default 64 schneidet lange Befehle (ota_url)

  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) {
    delay(10);
  }

  Serial.println();
  Serial.println(F("--------------------------------------------------"));
  Serial.println(F("moki · stage 1ab · epdiy + lvgl"));
  Serial.printf ("  ESP-IDF       : %s\n",  ESP.getSdkVersion());
  Serial.printf ("  Chip          : %s rev %d\n",
                 ESP.getChipModel(), (int)ESP.getChipRevision());
  Serial.printf ("  PSRAM (free)  : %u bytes\n", (unsigned)ESP.getFreePsram());
  Serial.printf ("  Flash size    : %u bytes\n", (unsigned)ESP.getFlashChipSize());
  Serial.println(F("--------------------------------------------------"));

  // I2C bus shared by GT911, TPS65185, PCF8563, BQ25896, BQ27220, PCA9535.
  // (39, 40) = (SDA, SCL) per LILYGO wiring.
  // PSRAM-Heap-Allokation für die großen Arrays (M8.5 DRAM-Optimierung).
  // Zusammen ~50KB DRAM frei für WiFi/LWIP/epdiy LUT.
  g_habits      = (moki_habit_t  *)heap_caps_calloc(MAX_HABITS,    sizeof(moki_habit_t),    MALLOC_CAP_SPIRAM);
  g_todos       = (moki_todo_t   *)heap_caps_calloc(MAX_TODOS,     sizeof(moki_todo_t),     MALLOC_CAP_SPIRAM);
  g_events      = (moki_event_t  *)heap_caps_calloc(MAX_EVENTS,    sizeof(moki_event_t),    MALLOC_CAP_SPIRAM);
  g_notes       = (moki_note_t   *)heap_caps_calloc(MAX_NOTES,     sizeof(moki_note_t),     MALLOC_CAP_SPIRAM);
  g_lora_msgs   = (moki_lora_msg_t*)heap_caps_calloc(LORA_MSG_CAP, sizeof(moki_lora_msg_t), MALLOC_CAP_SPIRAM);
  g_books       = (moki_book_t   *)heap_caps_calloc(MOKI_MAX_BOOKS,sizeof(moki_book_t),     MALLOC_CAP_SPIRAM);
  g_pins        = (moki_pin_t    *)heap_caps_calloc(MOKI_MAX_PINS, sizeof(moki_pin_t),      MALLOC_CAP_SPIRAM);
  g_map_proj_pts= (lv_point_t    *)heap_caps_calloc(2048,          sizeof(lv_point_t),      MALLOC_CAP_SPIRAM);
  if (!g_habits || !g_todos || !g_events || !g_notes ||
      !g_lora_msgs || !g_books || !g_pins || !g_map_proj_pts) {
    Serial.println(F("[boot] PSRAM-heap alloc FAILED — fatal"));
    while (1) delay(1000);
  }
  Serial.println(F("[boot] PSRAM-heap allocs OK"));

  Wire.begin(39, 40);

  // E-Paper init — board v7 + ED047TC1 panel.
  epd_init(&DEMO_BOARD, &ED047TC1, EPD_LUT_64K);
  epd_set_vcom(1560);                      // ED047TC1 VCOM in mV
  hl = epd_hl_init(WAVEFORM);
  epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);   // 540 wide × 960 tall (phone shape)

  Serial.printf("[epd] orientation: %d × %d\n",
                epd_rotated_display_width(), epd_rotated_display_height());

  // Sync the high-level state with the panel: set HL framebuffer to all-white
  // and push it via GC16 so subsequent transitions go through the proper LUT
  // from a known-white starting point. Plain epd_clear() bypasses the HL
  // state which leaves it inconsistent and dampens contrast on next update.
  epd_hl_set_all_white(&hl);
  epd_poweron();
  epd_clear();                                                     // panel-level clear
  temperature = epd_ambient_temperature();
  check_err(epd_hl_update_screen(&hl, MODE_GC16, temperature));    // syncs HL state
  epd_poweroff();
  Serial.printf("[epd] ambient: %d°C\n", temperature);

  // LVGL on top.
  Serial.println(F("[lvgl] init"));
  lv_port_disp_init();

  // GT911 touch — must come AFTER Wire.begin() above. The driver re-uses our
  // bus; setPins handles the RST/INT timing dance that fixes the address at
  // 0x5D. Failure here is non-fatal — we just lose touch input.
  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  if (!touch.begin(Wire, GT911_SLAVE_ADDRESS_L, TOUCH_SDA, TOUCH_SCL)) {
    Serial.println(F("[touch] GT911 init FAILED — check wiring"));
  } else {
    Serial.println(F("[touch] GT911 ready @ 0x5D"));
    touch.setInterruptMode(LOW_LEVEL_QUERY);
    touch.setMaxTouchPoint(1);

    // LVGL pointer indev — only register if touch came up cleanly.
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);
    Serial.println(F("[lvgl] indev registered"));
  }

  // RTC (PCF85063) — same I2C bus as GT911. Boot-read + auto-seed if blank.
  rtc_init();

  // Synthetic indev — host-driven via Serial commands (tap X Y / long X Y).
  static lv_indev_drv_t synth_drv;
  lv_indev_drv_init(&synth_drv);
  synth_drv.type    = LV_INDEV_TYPE_POINTER;
  synth_drv.read_cb = synth_read_cb;
  lv_indev_drv_register(&synth_drv);
  Serial.println(F("[lvgl] synth indev registered"));

  fs_mount();
  // sd_mount muss NACH lora_init laufen — sonst korrumpiert ein
  // fehlgeschlagener SD-Mount den geteilten SPI-Bus, und SX1262
  // antwortet nicht mehr (CHIP_NOT_FOUND).
  // sd_mount() wird weiter unten gerufen, NACH lora_init().
  books_scan();   // läuft erstmal auf LittleFS only (SD kommt nach lora_init)
  map_load();
  pins_load();
  self_pos_load();
#ifdef MOKI_WIFI
  // WiFi-Creds laden, Init kommt aber erst nach lora_init() — sonst stört
  // der WiFi-Stack die SPI/Power-Sequenz für SX1262 (CHIP_NOT_FOUND).
  wifi_load();
  ota_url_load();
#endif

  gps_init();

  state_load_todos();
  state_load_habits();
  state_load_mood();
  state_load_notes();
  state_load_settings();
  state_load_events();
  state_load_identity();
  state_load_book_page();
  state_load_channels();    // multi-channel list (Phase 1 of M4)
  lora_persist_load();   // restore last LORA_MSG_CAP messages from disk

  lora_init();   // safe RX-only by default
  // After radio is up, apply user-chosen preset (Moki / MeshCore / Meshtastic).
  if (g_lora_ready) {
    lora_apply_preset(g_settings.lora_preset);
  }

  // SD jetzt mounten — der LoRa-Radio teilt sich den SPI-Bus mit der SD-Karte.
  // Wenn SD vor LoRa initialisiert wird und ohne Karte fehlschlägt, bleibt
  // der Bus in einem komischen Zustand und SX1262 antwortet nicht mehr.
  sd_mount();
  // Falls SD jetzt da ist, indexieren wir die Bücher dort nach.
  if (g_sd_ready) books_scan();

#ifdef MOKI_WIFI
  // WiFi-Init NACH lora_init: Reihenfolge ist wichtig — wenn WiFi vor SX1262
  // hochkommt, klemmt's beim Radio-Begin (CHIP_NOT_FOUND).
  wifi_init();
#endif
  // Identity needs RNG → must come after radio is up. First boot only.
  state_ensure_identity();

  // M7 — initialize MeshCore mesh layer. Identity is now in NVS, radio is up,
  // so we can build the BaseChatMesh-derived MyMesh and start advertising.
  // NOTE: takes over RX from our raw lora_poll path. Foreign-protocol scan
  // commands (lora_preset_meshtastic_*, etc.) are still callable but they
  // reconfigure the radio underneath the mesh — use with care.
  if (g_lora_ready && g_identity_ready) {
    moki_mesh_init();
  }

  Serial.println(F("[lvgl] ui_entry"));
  ui_entry();
}

// ----------------------------------------------------------------------------
// Serial command parser — host can drive UI tests by sending lines:
//   tap X Y            — synthetic 300ms press at (X,Y)
//   long X Y           — synthetic 700ms long-press at (X,Y)
//   dump               — print current screen + heap snapshot
// ----------------------------------------------------------------------------
static void emit_synth_tap(int16_t x, int16_t y, uint32_t hold_ms) {
  synth_x = x;
  synth_y = y;
  synth_pressed = true;
  synth_release_at_ms = millis() + hold_ms;
  Serial.printf("[synth] press (%d,%d) hold=%lums\n",
                x, y, (unsigned long)hold_ms);
}

static String g_serial_buf;
static void poll_serial(void) {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = g_serial_buf;
      g_serial_buf = "";
      line.trim();
      if (line.length() == 0) continue;

      if (line.startsWith("tap ") || line.startsWith("long ")) {
        bool is_long = line.startsWith("long ");
        int p1 = line.indexOf(' ');
        int p2 = line.indexOf(' ', p1 + 1);
        if (p1 > 0 && p2 > p1) {
          int x = line.substring(p1 + 1, p2).toInt();
          int y = line.substring(p2 + 1).toInt();
          emit_synth_tap((int16_t)x, (int16_t)y, is_long ? 700 : 300);
        }
      } else if (line.startsWith("goto ")) {
        int n = line.substring(5).toInt();
        switch_screen((screen_id_t)n);
      } else if (line == "lora") {
        Serial.printf("[lora] status='%s' rx=%lu tx=%lu armed=%d msgs=%d\n",
                      g_lora_status,
                      (unsigned long)g_lora_rx_count,
                      (unsigned long)g_lora_tx_count,
                      g_settings.lora_tx_armed ? 1 : 0,
                      g_lora_msg_count);
      } else if (line.startsWith("lorasend ")) {
        // Inject a fake received message (for UI testing without a 2nd device)
        const String text = line.substring(9);
        lora_push_msg("test", text.c_str(), -42);
        Serial.println(F("[lora] injected fake rx"));
      } else if (line == "loraarm") {
        g_settings.lora_tx_armed = true; state_save_settings();
        Serial.println(F("[lora] tx armed via serial"));
      } else if (line == "loradisarm") {
        g_settings.lora_tx_armed = false; state_save_settings();
        Serial.println(F("[lora] tx disarmed"));
      } else if (line.startsWith("lora_freq ")) {
        float f = line.substring(10).toFloat();
        g_radio.standby(); g_radio.setFrequency(f); g_radio.startReceive();
        Serial.printf("[lora] freq=%.3fMHz\n", f);
      } else if (line.startsWith("lora_bw ")) {
        float bw = line.substring(8).toFloat();
        g_radio.standby(); g_radio.setBandwidth(bw); g_radio.startReceive();
        Serial.printf("[lora] bw=%.1fkHz\n", bw);
      } else if (line.startsWith("lora_sf ")) {
        int sf = line.substring(8).toInt();
        g_radio.standby(); g_radio.setSpreadingFactor(sf); g_radio.startReceive();
        Serial.printf("[lora] sf=%d\n", sf);
      } else if (line.startsWith("lora_cr ")) {
        int cr = line.substring(8).toInt();
        g_radio.standby(); g_radio.setCodingRate(cr); g_radio.startReceive();
        Serial.printf("[lora] cr=4/%d\n", cr);
      } else if (line.startsWith("lora_sync ")) {
        // Accept "0x12" or "12" (hex)
        String s = line.substring(10);
        s.trim();
        if (s.startsWith("0x") || s.startsWith("0X")) s = s.substring(2);
        uint8_t sync = (uint8_t)strtol(s.c_str(), NULL, 16);
        g_radio.standby(); g_radio.setSyncWord(sync); g_radio.startReceive();
        Serial.printf("[lora] sync=0x%02X\n", sync);
      } else if (line.startsWith("lora_crc ")) {
        bool on = line.substring(9) == "on" || line.substring(9) == "1";
        g_radio.standby(); g_radio.setCRC(on); g_radio.startReceive();
        Serial.printf("[lora] crc=%d\n", on ? 1 : 0);
      } else if (line == "lora_preset_meshtastic_lf") {
        // Meshtastic Long Fast EU (default in Heidelberg most likely)
        lora_reconfigure(869.525, 250.0, 11, 5, 0x2B, true);
      } else if (line == "lora_preset_meshtastic_ls") {
        // Meshtastic Long Slow
        lora_reconfigure(869.525, 125.0, 12, 8, 0x2B, true);
      } else if (line == "lora_preset_meshtastic_mf") {
        // Meshtastic Medium Fast
        lora_reconfigure(869.525, 250.0, 9, 5, 0x2B, true);
      } else if (line == "lora_preset_meshcore") {
        // MeshCore EU "Narrow" preset (current standard since Oct 2025)
        lora_reconfigure(869.618, 62.5, 8, 8, 0x12, true);
      } else if (line == "lora_preset_meshcore_old") {
        // MeshCore EU pre-2025 (legacy SF11)
        lora_reconfigure(869.525, 250.0, 11, 5, 0x12, true);
      } else if (line == "lora_preset_moki") {
        // Moki defaults
        lora_reconfigure(868.0, 250.0, 10, 6, 0xAB, true);
      } else if (line == "lora_status") {
        Serial.printf("[lora] ready=%d rx=%lu tx=%lu armed=%d\n",
                      g_lora_ready ? 1 : 0,
                      (unsigned long)g_lora_rx_count,
                      (unsigned long)g_lora_tx_count,
                      g_settings.lora_tx_armed ? 1 : 0);
      } else if (line == "lora_dump") {
        // Print every message in the ring buffer (oldest first), with hex.
        // Format per line:
        //   [%2d] +%lus rssi=%d snr=%.1f len=%u preset=%s from='%s' raw=<hex>
        Serial.printf("[lora_dump] %d msgs (cap=%d)\n",
                      g_lora_msg_count, LORA_MSG_CAP);
        int start = (g_lora_msg_count == LORA_MSG_CAP) ? g_lora_msg_head : 0;
        uint32_t now = millis();
        for (int n = 0; n < g_lora_msg_count; n++) {
          int idx = (start + n) % LORA_MSG_CAP;
          const moki_lora_msg_t *m = &g_lora_msgs[idx];
          // Pre-reboot messages have ts_ms > now → would underflow.
          // Print "older" instead of bogus large negatives.
          int64_t diff = (int64_t)now - (int64_t)m->ts_ms;
          if (diff < 0) {
            Serial.printf("  [%2d] older    rssi=%d snr=%.1f len=%u preset=%s from='%s' raw=",
                          n, m->rssi, m->snr_x10/10.0,
                          (unsigned)m->len,
                          (m->preset < LORA_PRESET_COUNT
                           ? LORA_PRESET_LABELS[m->preset] : "?"),
                          m->from);
            for (int i = 0; i < m->len; i++) Serial.printf("%02x", (uint8_t)m->text[i]);
            Serial.println();
            continue;
          }
          uint32_t age_s = (uint32_t)(diff / 1000);
          Serial.printf("  [%2d] -%lus rssi=%d snr=%.1f len=%u preset=%s from='%s' raw=",
                        n, (unsigned long)age_s, m->rssi, m->snr_x10/10.0,
                        (unsigned)m->len,
                        (m->preset < LORA_PRESET_COUNT
                         ? LORA_PRESET_LABELS[m->preset] : "?"),
                        m->from);
          for (int i = 0; i < m->len; i++) Serial.printf("%02x", (uint8_t)m->text[i]);
          Serial.println();
        }
        Serial.println(F("[lora_dump] done"));
      } else if (line.startsWith("mesh_send ")) {
        // mesh_send <text> — push a channel message into the MeshCore mesh.
        const String text = line.substring(10);
        if (!g_settings.lora_tx_armed) {
          Serial.println(F("[mesh] tx not armed — set ANTENNE DRAN in settings first"));
        } else {
          bool ok = moki_mesh_send(g_settings.handle, text.c_str());
          Serial.printf("[mesh] send %s\n", ok ? "queued" : "FAILED");
        }
      } else if (line.startsWith("sleep_now")) {
        // Format: "sleep_now [SECS]" — secs default 60
        int secs = 60;
        int sp = line.indexOf(' ');
        if (sp > 0) secs = line.substring(sp + 1).toInt();
        if (secs < 5) secs = 5;
        if (secs > 3600) secs = 3600;
        Serial.printf("[sleep] requested %ds via serial\n", secs);
        enter_deep_sleep((uint32_t)secs);
        // never returns
      } else if (line == "gps") {
        Serial.printf("[gps] fix=%d ant=%s used=%u track=%u hdop=%.1f lat=%.6f lon=%.6f age=%lums\n",
                      g_gps.fix ? 1 : 0,
                      g_gps.antenna_ok ? "ok" : "?",
                      (unsigned)g_gps.sats_used,
                      (unsigned)g_gps.sats_tracked,
                      g_gps.hdop_x10 / 10.0f,
                      g_gps.lat_deg, g_gps.lon_deg,
                      g_gps.fix ? (unsigned long)(millis() - g_gps.fix_ts_ms) : 0UL);
      } else if (line == "zoom_in") {
        if (g_viewport.zoom_idx < MAP_Z_CITY) {
          g_viewport.zoom_idx++;
          if (g_map_obj) lv_obj_invalidate(g_map_obj);
        }
        Serial.printf("[map] zoom=%d\n", (int)g_viewport.zoom_idx);
      } else if (line == "zoom_out") {
        if (g_viewport.zoom_idx > MAP_Z_COUNTRY) {
          g_viewport.zoom_idx--;
          if (g_map_obj) lv_obj_invalidate(g_map_obj);
        }
        Serial.printf("[map] zoom=%d\n", (int)g_viewport.zoom_idx);
      } else if (line.startsWith("pin_add ")) {
        // pin_add <name> — fügt Pin am aktuellen Viewport-Center ein
        String name = line.substring(8);
        if (pins_add(g_viewport.lat_center, g_viewport.lon_center, name.c_str())) {
          Serial.printf("[pin] added '%s' at %.4f,%.4f\n",
                        name.c_str(), g_viewport.lat_center, g_viewport.lon_center);
          if (g_map_obj) lv_obj_invalidate(g_map_obj);
        } else Serial.println(F("[pin] add failed"));
      } else if (line == "pin_list") {
        Serial.printf("[pin] %u pins:\n", (unsigned)g_pins_n);
        for (uint8_t i = 0; i < g_pins_n; i++) {
          Serial.printf("  [%u] %.4f,%.4f  %s\n",
                        (unsigned)i, g_pins[i].lat, g_pins[i].lon, g_pins[i].name);
        }
      } else if (line == "pin_clear") {
        g_pins_n = 0;
        pins_save();
        Serial.println(F("[pin] cleared"));
        if (g_map_obj) lv_obj_invalidate(g_map_obj);
      } else if (line.startsWith("pin_rename ")) {
        // pin_rename <idx> <name>
        int sp = line.indexOf(' ', 11);
        if (sp > 11) {
          int idx = line.substring(11, sp).toInt();
          String nm = line.substring(sp + 1);
          if (pins_rename(idx, nm.c_str())) {
            Serial.printf("[pin] renamed %d → '%s'\n", idx, nm.c_str());
            if (g_map_obj) lv_obj_invalidate(g_map_obj);
          }
        }
      } else if (line.startsWith("pin_note ")) {
        // pin_note <idx> <text>
        int sp = line.indexOf(' ', 9);
        if (sp > 9) {
          int idx = line.substring(9, sp).toInt();
          String nt = line.substring(sp + 1);
          if (pins_set_note(idx, nt.c_str())) {
            Serial.printf("[pin] note %d set\n", idx);
          }
        }
      } else if (line.startsWith("pin_share ")) {
        // pin_share <contact_idx> <pin_idx>  — sendet pin per DM an Kontakt
        String rest = line.substring(10);
        int sp = rest.indexOf(' ');
        if (sp <= 0) {
          Serial.println(F("[pin] usage: pin_share <contact_idx> <pin_idx>"));
        } else {
          int contact_idx = rest.substring(0, sp).toInt();
          int pin_idx     = rest.substring(sp + 1).toInt();
          if (pin_idx < 0 || pin_idx >= (int)g_pins_n) {
            Serial.printf("[pin] no pin %d (have %u)\n", pin_idx, (unsigned)g_pins_n);
          } else if (!g_settings.lora_tx_armed) {
            Serial.println(F("[pin] tx not armed"));
          } else {
            String payload = pin_share_format(&g_pins[pin_idx]);
            bool ok = moki_mesh_dm(contact_idx, payload.c_str());
            Serial.printf("[pin] share '%s' → contact[%d] %s (%u bytes)\n",
                          g_pins[pin_idx].name, contact_idx,
                          ok ? "queued" : "FAILED",
                          (unsigned)payload.length());
          }
        }
      } else if (line.startsWith("pin_del ")) {
        int idx = line.substring(8).toInt();
        if (pins_delete(idx)) {
          Serial.printf("[pin] deleted %d\n", idx);
          if (g_map_obj) lv_obj_invalidate(g_map_obj);
        }
#ifdef MOKI_WIFI
      } else if (line.startsWith("wifi_set ")) {
        // wifi_set <ssid> <psk>  — psk darf Leerzeichen enthalten ab 2. Wort
        String rest = line.substring(9);
        rest.trim();
        int sp = rest.indexOf(' ');
        if (sp <= 0) {
          Serial.println(F("[wifi] usage: wifi_set <ssid> <psk>"));
        } else {
          String ssid = rest.substring(0, sp);
          String psk  = rest.substring(sp + 1);
          wifi_set_creds(ssid.c_str(), psk.c_str());
        }
      } else if (line == "wifi_clear") {
        wifi_clear_creds();
      } else if (line == "wifi_scan") {
        // Debug: WiFi.scanNetworks + Liste in Serial dumpen
        if (WiFi.getMode() != WIFI_STA) {
          WiFi.mode(WIFI_STA);
          WiFi.onEvent(wifi_event_cb);
          delay(500);
        }
        WiFi.disconnect(false, true);
        delay(200);
        Serial.println(F("[wifi_scan] starting ..."));
        int n = WiFi.scanNetworks(false, false, false, 400);
        Serial.printf("[wifi_scan] found %d\n", n);
        for (int i = 0; i < n && i < 20; i++) {
          Serial.printf("  [%d] '%s'  rssi=%d  ch=%d  enc=%d\n", i,
                        WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                        WiFi.channel(i), WiFi.encryptionType(i));
        }
        WiFi.scanDelete();
      } else if (line == "wifi_status") {
        Serial.printf("[wifi] enabled=%d connected=%d ssid='%s' ip=%s rssi=%d\n",
                      g_wifi.enabled, g_wifi.connected,
                      g_wifi.ssid, g_wifi.ip[0] ? g_wifi.ip : "-",
                      g_wifi.connected ? WiFi.RSSI() : 0);
      } else if (line == "wifi_help" || line == "ota_help") {
        Serial.println(F("\n[ota] Quickstart:"));
        Serial.println(F("  WiFi setup: wifi_set <SSID> <PSK>  /  wifi_status"));
        Serial.println(F(""));
        Serial.println(F("  LOKAL (Mac im LAN):"));
        Serial.println(F("    Mac:  cd tools/ota-server && bun run start"));
        Serial.println(F("    Moki: ota http://<MAC-IP>:8080/firmware.bin"));
        Serial.println(F(""));
        Serial.println(F("  REMOTE (GitHub Releases — funktioniert von überall):"));
        Serial.println(F("    Mac:  ./tools/release.sh \"meine änderung\""));
        Serial.println(F("    Moki: ota_url https://github.com/<USER>/<REPO>/releases/latest/download/firmware.bin   (einmalig)"));
        Serial.println(F("    Moki: ota_release"));
        Serial.println(F(""));
        Serial.println(F("  Diagnose: 'info' (heap/wifi/gps/pins), 'ota_url_get' (URL anzeigen)."));
      } else if (line.startsWith("ota ")) {
        String url = line.substring(4);
        url.trim();
        if (url.length() < 8) {
          Serial.println(F("[ota] usage: ota <http(s)://host/firmware.bin>"));
        } else {
          ota_pull(url.c_str());
        }
      } else if (line == "ota_release") {
        if (!g_ota_url[0]) {
          Serial.println(F("[ota] no default URL — set with 'ota_url <url>' first"));
          Serial.println(F("[ota] tip: https://github.com/<user>/<repo>/releases/latest/download/firmware.bin"));
        } else {
          ota_pull(g_ota_url);
        }
      } else if (line.startsWith("ota_url ")) {
        String url = line.substring(8);
        url.trim();
        if (url.length() < 8) {
          Serial.println(F("[ota] usage: ota_url <url>  (set persistent default)"));
        } else {
          ota_url_save(url.c_str());
        }
      } else if (line == "ota_url_get") {
        Serial.printf("[ota] url='%s'\n", g_ota_url[0] ? g_ota_url : "(unset)");
      } else if (line == "ota_url_reset") {
        // NVS-Wert löschen → fällt auf compiled-in Default zurück
        g_prefs.begin("moki", false);
        g_prefs.remove("ota_url");
        g_prefs.end();
        strncpy(g_ota_url, OTA_DEFAULT_URL, sizeof(g_ota_url) - 1);
        g_ota_url[sizeof(g_ota_url) - 1] = 0;
        Serial.printf("[ota] reset → %s\n", g_ota_url);
#endif // MOKI_WIFI
      } else if (line == "info") {
        // Eine-Zeile-Snapshot für schnelle Diagnose
        Serial.printf("[info] uptime=%lus heap=%u psram=%u(free) ",
                      (unsigned long)(millis() / 1000),
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getFreePsram());
#ifdef MOKI_WIFI
        Serial.printf("wifi=%s ip=%s ",
                      g_wifi.connected ? "up" : (g_wifi.enabled ? "trying" : "off"),
                      g_wifi.ip[0] ? g_wifi.ip : "-");
#endif
        Serial.printf("gps=%s sats=%u/%u self=%s pins=%u\n",
                      g_gps.fix ? "fix" : "-",
                      (unsigned)g_gps.sats_tracked, (unsigned)g_gps.sats_used,
                      g_self_pos.source ? "yes" : "no",
                      (unsigned)g_pins_n);
      } else if (line == "pin_demo") {
        // Drei Demo-Pins rund um Heidelberg
        pins_add(49.4099f, 8.6943f, "schloss-cafe");
        pins_add(49.4196f, 8.6940f, "neckarwiese");
        pins_add(49.4045f, 8.6779f, "hbf");
        pins_set_note(g_pins_n - 3, "kaffee mit blick auf den neckar");
        pins_set_note(g_pins_n - 2, "lieblingsspot zum nachmittag");
        Serial.printf("[pin] +3 demo pins (total %u)\n", (unsigned)g_pins_n);
        if (g_map_obj) lv_obj_invalidate(g_map_obj);
      } else if (line == "restart" || line == "reboot") {
        Serial.println(F("[boot] manual restart"));
        delay(100);
        ESP.restart();
      } else if (line == "set_self_here") {
        self_pos_set_manual(g_viewport.lat_center, g_viewport.lon_center);
        Serial.printf("[self] manual set lat=%.4f lon=%.4f\n",
                      g_self_pos.lat, g_self_pos.lon);
        if (g_map_obj) lv_obj_invalidate(g_map_obj);
      } else if (line == "self") {
        Serial.printf("[self] source=%u lat=%.4f lon=%.4f\n",
                      (unsigned)g_self_pos.source, g_self_pos.lat, g_self_pos.lon);
      } else if (line == "center_me") {
        if (g_gps.fix) {
          g_viewport.lat_center = g_gps.lat_deg;
          g_viewport.lon_center = g_gps.lon_deg;
          if (g_map_obj) lv_obj_invalidate(g_map_obj);
          Serial.printf("[map] centered on GPS lat=%.4f lon=%.4f\n",
                        g_viewport.lat_center, g_viewport.lon_center);
        } else {
          Serial.println(F("[map] no GPS fix"));
        }
      } else if (line == "gps_raw") {
        // Dumpe 10s lang raw NMEA-Bytes vom Modul. Diagnostik wenn der
        // Parser nichts sieht oder etwas Komisches macht.
        Serial.println(F("[gps_raw] dumping raw NMEA for 10 seconds..."));
        uint32_t t_end = millis() + 10000;
        while (millis() < t_end) {
          while (GPSSerial.available()) {
            int c = GPSSerial.read();
            if (c >= 0) Serial.write((char)c);
          }
          delay(10);
        }
        Serial.println(F("\n[gps_raw] done"));
      } else if (line == "time") {
        if (g_rtc_ready) {
          RTC_DateTime now = g_rtc.getDateTime();
          Serial.printf("[rtc] %04u-%02u-%02u %02u:%02u:%02u\n",
                        now.year, now.month, now.day,
                        now.hour, now.minute, now.second);
        } else {
          Serial.println(F("[rtc] not ready"));
        }
      } else if (line.startsWith("set_autosleep ")) {
        int n = line.substring(14).toInt();
        if (n < 0 || n > 60) {
          Serial.println(F("[sleep] auto_sleep_min: 0=off, 1..60"));
        } else {
          g_settings.auto_sleep_min = (uint8_t)n;
          state_save_settings();
          mark_activity();   // reset countdown
          Serial.printf("[sleep] auto_sleep_min = %d\n", n);
        }
      } else if (line.startsWith("set_channel ")) {
        String name = line.substring(12);
        name.trim();
        if (name.length() == 0 || name.length() >= sizeof(g_settings.mesh_channel_name)) {
          Serial.println(F("[mesh] channel name 1..15 chars"));
        } else {
          strncpy(g_settings.mesh_channel_name, name.c_str(),
                  sizeof(g_settings.mesh_channel_name) - 1);
          g_settings.mesh_channel_name[sizeof(g_settings.mesh_channel_name) - 1] = 0;
          state_save_settings();
          Serial.printf("[mesh] channel name='%s' (effective on next boot)\n",
                        g_settings.mesh_channel_name);
        }
      } else if (line.startsWith("set_psk ")) {
        String psk = line.substring(8);
        psk.trim();
        if (psk.length() != 24) {
          Serial.println(F("[mesh] PSK must be 24-char Base64 (16 bytes raw)"));
        } else {
          strncpy(g_settings.mesh_channel_psk_b64, psk.c_str(),
                  sizeof(g_settings.mesh_channel_psk_b64) - 1);
          g_settings.mesh_channel_psk_b64[sizeof(g_settings.mesh_channel_psk_b64) - 1] = 0;
          state_save_settings();
          Serial.println(F("[mesh] PSK saved (effective on next boot)"));
        }
      } else if (line == "channel" || line == "ch_list") {
        Serial.printf("[mesh] %d channels (active=%u):\n",
                      g_num_channels, (unsigned)g_settings.mesh_active_channel);
        for (int i = 0; i < g_num_channels; i++) {
          Serial.printf("  [%d]%s '%s' psk='%s'\n",
                        i, i == g_settings.mesh_active_channel ? "*" : " ",
                        g_channels[i].name, g_channels[i].psk_b64);
        }
      } else if (line.startsWith("ch_add ")) {
        // ch_add <name> <psk_b64>
        String body = line.substring(7); body.trim();
        int sp = body.indexOf(' ');
        if (sp <= 0) {
          Serial.println(F("[mesh] usage: ch_add <name> <psk_b64>"));
        } else {
          String n = body.substring(0, sp); n.trim();
          String p = body.substring(sp + 1); p.trim();
          if (p.length() != 24) {
            Serial.println(F("[mesh] PSK must be 24-char Base64"));
          } else if (!channels_add(n.c_str(), p.c_str())) {
            Serial.println(F("[mesh] channel list full"));
          } else {
            Serial.printf("[mesh] added channel '%s' (effective on next boot)\n",
                          n.c_str());
          }
        }
      } else if (line.startsWith("ch_remove ")) {
        int idx = line.substring(10).toInt();
        if (channels_remove(idx)) {
          Serial.printf("[mesh] removed channel [%d] (effective on next boot)\n", idx);
        } else {
          Serial.println(F("[mesh] cannot remove (last/invalid)"));
        }
      } else if (line == "sd_list") {
        Serial.printf("[sd] ready=%d, %d books indexed:\n", g_sd_ready, g_book_count);
        for (int i = 0; i < g_book_count; i++) {
          Serial.printf("  [%d] %s  → %s\n", i, g_books[i].title, g_books[i].path);
        }
      } else if (line == "sd_rescan") {
        books_scan();
      } else if (line == "advert") {
        if (!g_settings.lora_tx_armed) {
          Serial.println(F("[mesh] tx not armed"));
        } else {
          moki_mesh_advert(g_settings.handle);
        }
      } else if (line == "contacts") {
        int n = moki_mesh_contact_count();
        Serial.printf("[mesh] %d contacts:\n", n);
        for (int i = 0; i < n; i++) {
          char name[40]; uint8_t key4[4];
          if (moki_mesh_get_contact(i, name, sizeof(name), key4)) {
            Serial.printf("  [%d] '%s' id=%02x%02x%02x%02x\n",
                          i, name, key4[0], key4[1], key4[2], key4[3]);
          }
        }
      } else if (line.startsWith("dm ")) {
        // dm <contact_idx> <text>
        String body = line.substring(3); body.trim();
        int sp = body.indexOf(' ');
        if (sp <= 0) {
          Serial.println(F("[mesh] usage: dm <idx> <text>"));
        } else if (!g_settings.lora_tx_armed) {
          Serial.println(F("[mesh] tx not armed"));
        } else {
          int idx = body.substring(0, sp).toInt();
          String text = body.substring(sp + 1);
          bool ok = moki_mesh_dm(idx, text.c_str());
          Serial.printf("[mesh] dm %s\n", ok ? "queued" : "FAILED");
        }
      } else if (line.startsWith("ch_active ")) {
        int idx = line.substring(10).toInt();
        if (idx < 0 || idx >= g_num_channels) {
          Serial.println(F("[mesh] active idx out of range"));
        } else {
          g_settings.mesh_active_channel = (uint8_t)idx;
          state_save_settings();
          Serial.printf("[mesh] active channel = [%d] '%s'\n",
                        idx, g_channels[idx].name);
        }
      } else if (line == "rtc_raw") {
        // Diagnostic: dump all 11 PCF85063 registers (0x00..0x0A) raw.
        Wire.beginTransmission(PCF85063_SLAVE_ADDRESS);
        Wire.write(0x00);
        if (Wire.endTransmission(false) == 0) {
          Wire.requestFrom(PCF85063_SLAVE_ADDRESS, (uint8_t)11);
          Serial.print(F("[rtc_raw] regs 00-0A: "));
          for (int i = 0; i < 11 && Wire.available(); i++) {
            Serial.printf("%02x ", Wire.read());
          }
          Serial.println();
        } else {
          Serial.println(F("[rtc_raw] no ACK from chip"));
        }
      } else if (line.startsWith("set_time ")) {
        // Format: "set_time YYYY-MM-DD HH:MM:SS"
        String body = line.substring(9);
        body.trim();
        RTC_DateTime t;
        if (rtc_parse(body, t) && g_rtc_ready) {
          // Use direct Wire bypass — SensorLib's setDateTime is broken
          // on our hardware (Bug Tracker entry).
          if (rtc_set_direct(t.year, t.month, t.day, t.hour, t.minute, t.second)) {
            Serial.printf("[rtc] set to %04u-%02u-%02u %02u:%02u:%02u\n",
                          t.year, t.month, t.day, t.hour, t.minute, t.second);
          }
        } else {
          Serial.println(F("[rtc] parse failed — expected YYYY-MM-DD HH:MM:SS"));
        }
      } else if (line == "lora_clear") {
        g_lora_msg_count = 0;
        g_lora_msg_head  = 0;
        g_lora_rx_count  = 0;
        g_lora_last_rssi  = 0;
        g_lora_last_snr   = 0.0f;
        g_lora_last_rx_ms = 0;
        lora_persist_clear();
        Serial.println(F("[lora] cleared ring buffer + counts + disk log"));
      } else if (line == "dump") {
        Serial.printf("[dump] screen=%d heap=%u psram=%u todos=%d habits=%d\n",
                      (int)current_screen,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getFreePsram(),
                      g_todos_count, g_habits_count);
        for (int i = 0; i < g_habits_count; i++) {
          Serial.printf("  habit[%d] '%s' today=%u streak=%u\n",
                        i, g_habits[i].name,
                        (unsigned)g_habits[i].today_count,
                        (unsigned)g_habits[i].streak);
        }
      }
    } else if (g_serial_buf.length() < 64) {
      g_serial_buf += c;
    }
  }

  // Auto-release synth press after hold timer expires.
  if (synth_pressed && (int32_t)(millis() - synth_release_at_ms) >= 0) {
    synth_pressed = false;
    Serial.printf("[synth] release\n");
  }
}

// ============================================================================
// M3 — Deep Sleep skeleton (testbar via Serial; Auto-Sleep optional/Off)
// ============================================================================
// Strategy:
//   - Save state to NVS/LittleFS (already happens on every change, no extra)
//   - Power down EPD via existing epdiy disable
//   - Configure GT911 IRQ as wake source (TOUCH_IRQ → ext0)
//   - Configure timer wakeup for periodic LoRa-burst-RX
//   - esp_deep_sleep_start() — chip resets on wake, full setup() runs again
//
// Auto-Sleep toggle is settings-controlled (default OFF) so we don't surprise
// Lucas. Manual `sleep_now SECS` Serial command for testing.
#include <esp_sleep.h>

static void prepare_for_sleep(void) {
  Serial.println(F("[sleep] preparing — save state + power down"));
  Serial.flush();
  // EPD content stays visible without power (e-paper persistent), so just
  // power the panel off cleanly.
  epd_poweroff();
  // LoRa: standby is enough — chip doesn't draw much in standby mode.
  if (g_lora_ready) g_radio.standby();
  // Touch: GT911 sleep command (drives INT low, low-power standby).
  touch.sleep();
}

static void enter_deep_sleep(uint32_t wake_after_seconds) {
  prepare_for_sleep();
  if (wake_after_seconds > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)wake_after_seconds * 1000000ULL);
  }
  // Touch IRQ wake — GT911 INT goes LOW on touch.
  // GPIO3 on ESP32-S3 is RTC-capable so ext0 works.
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_IRQ, 0);
  Serial.printf("[sleep] entering deep sleep, wake in %lus or on touch\n",
                (unsigned long)wake_after_seconds);
  Serial.flush();
  esp_deep_sleep_start();
  // Never returns.
}

void loop() {
  // LVGL ticker
  lv_timer_handler();

  // M7 — MeshCore owns the radio now. Its loop drives RX/TX/dispatch.
  // The legacy lora_poll() raw-radio path is *not* called when mesh is up,
  // because both would race on the same SX1262 IRQ + readData and corrupt
  // each other. lora_poll remains defined for the foreign-protocol scan
  // tools (lora_preset_meshtastic_*, etc.) — those need a separate "raw
  // mode" toggle in a future iteration.
  moki_mesh_loop();

  // GPS — drain Serial2 NMEA, update g_gps state
  gps_loop();

#ifdef MOKI_WIFI
  // WiFi — Reconnect-Watchdog
  wifi_loop();
#endif

  // RTC tick — habit midnight rollover (cheap, runs ~once / 30s)
  rtc_tick();

  // Periodic self-advert — fires once 5s after boot, then every 15 min, so
  // other Mokis can discover this one without manual `advert` commands.
  // Requires TX armed (otherwise we'd risk damage on antenna-less Mokis).
  // Wenn ein GPS-Fix da ist, senden wir die Position mit, sodass andere
  // Mokis uns auf der Karte sehen können.
  static uint32_t next_advert_ms = 5000;
  if (g_settings.lora_tx_armed && g_lora_ready &&
      millis() > next_advert_ms) {
    if (g_gps.fix) {
      moki_mesh_advert_with_loc(g_settings.handle,
                                (double)g_gps.lat_deg, (double)g_gps.lon_deg);
    } else {
      moki_mesh_advert(g_settings.handle);
    }
    next_advert_ms = millis() + 15UL * 60UL * 1000UL;  // every 15 min
  }

  // Host-driven synthetic touch + diagnostic commands
  poll_serial();

  // Direct touch poll — bypasses LVGL to confirm raw hardware state.
  static bool was_pressed = false;
  bool now_pressed = touch.isPressed();
  if (now_pressed != was_pressed) {
    Serial.printf("[touch poll] state %d → %d\n", was_pressed, now_pressed);
    was_pressed = now_pressed;
    if (now_pressed) mark_activity();
  }

  // Auto-sleep after configured idle — opt-in via g_settings.auto_sleep_min.
  if (g_settings.auto_sleep_min > 0 && g_last_activity_ms > 0) {
    uint32_t idle_ms = millis() - g_last_activity_ms;
    uint32_t threshold_ms = g_settings.auto_sleep_min * 60UL * 1000UL;
    if (idle_ms > threshold_ms) {
      Serial.printf("[sleep] auto-sleep after %lu min idle\n",
                    (unsigned long)g_settings.auto_sleep_min);
      // Sleep for sync_interval * 60s — wake to do a sync round.
      uint32_t wake_secs = g_settings.sync_interval_min * 60UL;
      enter_deep_sleep(wake_secs);
    }
  }

  // Heartbeat once per second.
  static uint32_t last_beat = 0;
  uint32_t now = millis();
  if (now - last_beat >= 1000) {
    last_beat = now;
    Serial.printf("moki alive · t=%lus · heap=%u · psram=%u\n",
                  (unsigned long)(now / 1000),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram());
  }

  // GPS heartbeat — alle 10s ausgeben, damit Lucas draußen das Fix-Verhalten
  // im Serial sieht. Hört auf, sobald Status-Bar GPS anzeigt.
  static uint32_t last_gps_beat = 0;
  if (now - last_gps_beat >= 10000) {
    last_gps_beat = now;
    if (g_gps.fix) {
      Serial.printf("[gps] FIX  lat=%.6f lon=%.6f used=%u track=%u hdop=%.1f\n",
                    g_gps.lat_deg, g_gps.lon_deg,
                    (unsigned)g_gps.sats_used,
                    (unsigned)g_gps.sats_tracked,
                    g_gps.hdop_x10 / 10.0f);
    } else {
      Serial.printf("[gps] no fix · tracking %u sats · ant=%s · hdop=%.1f\n",
                    (unsigned)g_gps.sats_tracked,
                    g_gps.antenna_ok ? "ok" : "?",
                    g_gps.hdop_x10 / 10.0f);
    }
  }
  delay(1);
}
