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
#include "moki_fonts.h"

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
  uint8_t day; const char *hour; const char *title; const char *place;
  const char *kind;       // private/friends/public
} moki_event_t;

struct sample_habit_def { const char *name; uint8_t today; uint8_t streak; float intensity; };
static const sample_habit_def SAMPLE_HABITS_DEF[] = {
  { "Lesen · 10 min", 1, 4, 0.65f },
  { "Spaziergang",    2, 7, 0.85f },
  { "Wasser",         3, 2, 0.90f },
  { "Tagebuch",       0, 0, 0.30f },
};
static const int SAMPLE_HABITS_COUNT = sizeof(SAMPLE_HABITS_DEF) / sizeof(SAMPLE_HABITS_DEF[0]);

#define MAX_HABITS 16
static moki_habit_t g_habits[MAX_HABITS];
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
static moki_todo_t g_todos[MAX_TODOS];
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
  uint32_t n = g_prefs.getUInt("todos_n", 0xFFFFFFFFu);
  if (n != 0xFFFFFFFFu && n <= MAX_TODOS) {
    g_todos_count = (int)n;
    if (g_todos_count > 0) {
      g_prefs.getBytes("todos", g_todos, sizeof(moki_todo_t) * g_todos_count);
    }
    g_prefs.end();
    Serial.printf("[persist] loaded %d todos from NVS\n", g_todos_count);
  } else {
    g_prefs.end();
    Serial.println(F("[persist] NVS empty — bootstrapping with sample data"));
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
static moki_note_t g_notes[MAX_NOTES];
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
               SCR_SETTINGS } screen_id_t;

// Settings (persistent)
typedef struct {
  uint8_t  sync_interval_min;     // 5 / 15 / 30 / 60
  char     share_default[8];      // off / hourly / live
  char     handle[32];            // user handle
  char     bio[80];
} moki_settings_t;
static moki_settings_t g_settings = { 30, "hourly", "levin", "liest langsam. läuft lieber." };

static void state_save_settings(void) {
  g_prefs.begin("moki", false);
  g_prefs.putBytes("settings", &g_settings, sizeof(g_settings));
  g_prefs.end();
  Serial.println(F("[persist] saved settings"));
}
static void state_load_settings(void) {
  g_prefs.begin("moki", true);
  size_t got = g_prefs.getBytes("settings", &g_settings, sizeof(g_settings));
  g_prefs.end();
  if (got == sizeof(g_settings)) {
    Serial.printf("[persist] loaded settings (sync=%u handle='%s')\n",
                  (unsigned)g_settings.sync_interval_min, g_settings.handle);
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
typedef enum { COMPOSE_TODO, COMPOSE_HABIT } compose_kind_t;
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
  } else { // COMPOSE_HABIT
    if (g_habits_count >= MAX_HABITS) return;
    moki_habit_t *h = &g_habits[g_habits_count++];
    strncpy(h->name, g_compose_title, sizeof(h->name)-1); h->name[sizeof(h->name)-1] = 0;
    h->today_count = 0;
    h->streak      = 0;
    Serial.printf("[compose] saved habit: '%s'\n", h->name);
    state_save_habits();
    show_toast("GEWOHNHEIT GESPEICHERT");
  }

  g_compose_title[0]    = 0;
  strcpy(g_compose_cat, "self");
  g_compose_deadline[0] = 0;
  g_compose_recurring   = false;
  compose_close();
  // Re-arm the DO screen so we pick the right tab
  current_do_tab = (g_compose_kind == COMPOSE_TODO) ? DO_TODOS : DO_HABITS;
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
  lv_label_set_text(t, g_compose_kind == COMPOSE_TODO ? "NEUE AUFGABE" : "NEUE GEWOHNHEIT");
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
  }
  lv_disp_flush_ready(disp);
}

static void flush_timer_cb(lv_timer_t *t) {
  EpdRect render_area = {
      .x = 0, .y = 0,
      .width  = (int)epd_rotated_display_width(),
      .height = (int)epd_rotated_display_height(),
  };
  epd_draw_rotated_image(render_area, decodebuffer, epd_hl_get_framebuffer(&hl));
  epd_poweron();
  // MODE_GC16 = 16-level greyscale, slow but full-quality. Anti-aliased text
  // needs the grey shades; MODE_DU collapses them inconsistently.
  check_err(epd_hl_update_screen(&hl, MODE_GC16, epd_ambient_temperature()));
  epd_poweroff();
  Serial.println(F("[epd] flush done (MODE_GC16)"));
  lv_timer_pause(flush_timer);
}

static void disp_render_start_cb(struct _lv_disp_drv_t *disp_drv) {
  if (flush_timer == NULL) {
    flush_timer = lv_timer_create(flush_timer_cb, 200, NULL);
    lv_timer_ready(flush_timer);
  } else {
    lv_timer_resume(flush_timer);
  }
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

  lv_obj_t *sync = lv_label_create(bar);
  lv_label_set_text(sync, "SYNC · 12M");        // · = U+00B7
  lv_obj_set_style_text_color(sync, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_font(sync, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sync, 2, LV_PART_MAIN);
  lv_obj_add_flag(sync, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(sync, on_element_tapped, LV_EVENT_CLICKED, (void *)"sync");

  lv_obj_t *right = lv_label_create(bar);
  lv_label_set_text(right, "78  14:32");
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
  lv_obj_add_event_cb(tile, on_element_tapped,
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
  build_stat_tile(tiles, "GEWOHN", "1/4", "heute",      "stat-gewohn");
  build_stat_tile(tiles, "AUFGAB", "3",   "offen",      "stat-aufgab");
  build_stat_tile(tiles, "NAH",    "3",   "in der nähe", "stat-nah");
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
    case SCR_SETTINGS:    { extern void build_settings(void);    build_settings();    break; }
  }
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

static int g_active_chat = -1;
static void on_chat_open(lv_event_t *e) {
  g_active_chat = (int)(intptr_t)lv_event_get_user_data(e);
  switch_screen(SCR_CHAT_DETAIL);
}
static void on_chat_back(lv_event_t *e) {
  g_active_chat = -1;
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

    for (int i = 0; i < SAMPLE_EVENTS_COUNT; i++) {
      const moki_event_t *ev = &SAMPLE_EVENTS[i];
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
    lv_obj_t *author = lv_label_create(col);
    lv_label_set_text(author, "HENRY DAVID THOREAU");
    lv_obj_set_style_text_font(author, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(author, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(author, 3, LV_PART_MAIN);
    lv_obj_set_style_text_align(author, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(author, LV_PCT(100));

    lv_obj_t *book = lv_label_create(col);
    lv_label_set_text(book, "Walden");
    lv_obj_set_style_text_font(book, &moki_fraunces_italic_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(book, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_align(book, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(book, LV_PCT(100));

    lv_obj_t *excerpt = lv_label_create(col);
    lv_label_set_text(excerpt,
      "Ich ging in die Wälder, weil ich mit Bedacht leben wollte, "
      "um nur den wesentlichen Tatsachen des Lebens ins Auge zu sehen, "
      "und zu lernen, was es zu lehren hatte — und nicht, wenn es zum "
      "Sterben käme, zu entdecken, dass ich nicht gelebt hatte.");
    lv_obj_set_style_text_font(excerpt, &moki_fraunces_italic_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(excerpt, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_label_set_long_mode(excerpt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(excerpt, LV_PCT(100));
    lv_obj_set_style_text_line_space(excerpt, 6, LV_PART_MAIN);

    // Page nav
    lv_obj_t *nav = lv_obj_create(col);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, LV_PCT(100), 40);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(nav, lv_color_hex(MOKI_MID), LV_PART_MAIN);
    lv_obj_set_style_border_width(nav, 1, LV_PART_MAIN);

    lv_obj_t *prev = lv_label_create(nav);
    lv_label_set_text(prev, "← ZURÜCK");
    lv_obj_set_style_text_font(prev, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(prev, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(prev, 2, LV_PART_MAIN);

    lv_obj_t *page = lv_label_create(nav);
    lv_label_set_text(page, "42 / 312");
    lv_obj_set_style_text_font(page, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(page, lv_color_hex(MOKI_INK), LV_PART_MAIN);

    lv_obj_t *next = lv_label_create(nav);
    lv_label_set_text(next, "WEITER →");
    lv_obj_set_style_text_font(next, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(next, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(next, 2, LV_PART_MAIN);
  } else if (current_read_tab == READ_FEED) {
    lv_obj_t *stub = lv_label_create(col);
    lv_label_set_text(stub, "feed kommt bald.");
    lv_obj_set_style_text_font(stub, &moki_fraunces_italic_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(stub, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_align(stub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(stub, LV_PCT(100));
  } else { // READ_NOTES
    // Pinned-first ordering
    int order[MAX_NOTES]; int o = 0;
    for (int i = 0; i < g_notes_count; i++) if (g_notes[i].pinned) order[o++] = i;
    for (int i = 0; i < g_notes_count; i++) if (!g_notes[i].pinned) order[o++] = i;

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

  // Back row
  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, LV_PCT(100), 36);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, on_habit_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "← ZURÜCK");
  lv_obj_set_style_text_font(bl, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(bl, 2, LV_PART_MAIN);

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

  // 12-week × 7-day heatmap grid
  lv_obj_t *grid = lv_obj_create(scr);
  lv_obj_remove_style_all(grid);
  lv_obj_set_size(grid, LV_PCT(100), 230);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  for (int w = 0; w < 12; w++) {
    lv_obj_t *col = lv_obj_create(grid);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 32, 230);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 3, LV_PART_MAIN);

    for (int d = 0; d < 7; d++) {
      int sample = h->history[w * 7 + d];
      if (sample > 4) sample = 4;
      lv_obj_t *cell = lv_obj_create(col);
      lv_obj_remove_style_all(cell);
      lv_obj_set_size(cell, 30, 30);
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

  lv_obj_t *title = lv_label_create(col);
  lv_label_set_text(title, "ein kleiner kreis.");
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(title, 8, LV_PART_MAIN);

  for (int i = 0; i < SAMPLE_CHATS_COUNT; i++) {
    build_chat_row(col, &SAMPLE_CHATS[i]);
  }
}

static void build_chats(void) {
  lv_obj_t *content = build_screen_chrome(lv_scr_act(), 3);
  build_chats_content(content);
}

// ----------------------------------------------------------------------------
// MAP — stylized cartography + "in der nähe" peer list
// ----------------------------------------------------------------------------
static void build_map_canvas(lv_obj_t *parent) {
  // Container with grid background and pin overlay. Map is sized to fit the
  // remaining vertical space; LVGL's flex_grow handles the height.
  lv_obj_t *map = lv_obj_create(parent);
  lv_obj_remove_style_all(map);
  lv_obj_set_size(map, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(map, 1);
  lv_obj_set_style_bg_color(map, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(map, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(map, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_border_width(map, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(map, 2, LV_PART_MAIN);

  // River line (Neckar, spiritually) — a wide MID rectangle approximating
  // the curved path from the simulator. Keeps it simple without canvas.
  lv_obj_t *river = lv_obj_create(map);
  lv_obj_remove_style_all(river);
  lv_obj_set_size(river, LV_PCT(120), 8);
  lv_obj_align(river, LV_ALIGN_LEFT_MID, -20, 80);
  lv_obj_set_style_bg_color(river, lv_color_hex(MOKI_MID), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(river, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(river, LV_RADIUS_CIRCLE, LV_PART_MAIN);

  // Place pins
  for (int i = 0; i < SAMPLE_PLACES_COUNT; i++) {
    const moki_place_t *p = &SAMPLE_PLACES[i];
    lv_obj_t *pin = lv_obj_create(map);
    lv_obj_remove_style_all(pin);
    lv_obj_set_size(pin, 14, 14);
    lv_obj_set_style_bg_color(pin, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(pin, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_border_width(pin, 2, LV_PART_MAIN);
    lv_obj_align(pin, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_pos(pin, (int)(p->x * 4.5f), (int)(p->y * 5.0f));

    lv_obj_t *lbl = lv_label_create(map);
    lv_label_set_text(lbl, p->name);
    lv_obj_set_style_text_font(lbl, &moki_fraunces_italic_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(lbl, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(lbl, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_right(lbl, 4, LV_PART_MAIN);
    lv_obj_set_pos(lbl, (int)(p->x * 4.5f) + 18, (int)(p->y * 5.0f) - 8);
  }

  // Friend-live pins
  for (int i = 0; i < SAMPLE_FRIENDS_LIVE_COUNT; i++) {
    const moki_friend_live_t *f = &SAMPLE_FRIENDS_LIVE[i];
    lv_obj_t *pin = lv_obj_create(map);
    lv_obj_remove_style_all(pin);
    lv_obj_set_size(pin, 16, 16);
    lv_obj_set_style_bg_color(pin, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(pin, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_pos(pin, (int)(f->x * 4.5f), (int)(f->y * 5.0f));

    char buf[32]; snprintf(buf, sizeof(buf), "%s · %s", f->name, f->fresh);
    lv_obj_t *lbl = lv_label_create(map);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &moki_fraunces_italic_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(lbl, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(lbl, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_right(lbl, 4, LV_PART_MAIN);
    lv_obj_set_pos(lbl, (int)(f->x * 4.5f) + 22, (int)(f->y * 5.0f) - 8);
  }

  // Self pin (filled INK with PAPER ring) — center, slightly below middle
  lv_obj_t *self_outer = lv_obj_create(map);
  lv_obj_remove_style_all(self_outer);
  lv_obj_set_size(self_outer, 22, 22);
  lv_obj_set_style_bg_color(self_outer, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(self_outer, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(self_outer, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(self_outer, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(self_outer, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_align(self_outer, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *self_inner = lv_obj_create(self_outer);
  lv_obj_remove_style_all(self_inner);
  lv_obj_set_size(self_inner, 12, 12);
  lv_obj_set_style_bg_color(self_inner, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(self_inner, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(self_inner, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_center(self_inner);
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
    lv_label_set_text(share, "ICH · STÜNDLICH");
    lv_obj_set_style_text_font(share, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(share, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(share, 2, LV_PART_MAIN);

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
    const char *txt = line.c_str();
    int color = MOKI_INK;
    if (line.startsWith("# ")) {
      font = &moki_fraunces_italic_36;
      txt = line.c_str() + 2;
    } else if (line.startsWith("## ")) {
      font = &moki_fraunces_regular_36;
      txt = line.c_str() + 3;
    } else if (line.startsWith("### ")) {
      font = &moki_fraunces_italic_28;
      txt = line.c_str() + 4;
    } else if (line.startsWith("- ")) {
      // bullet rendered with ASCII fallback (• missing in JB Mono)
      font = &moki_fraunces_italic_22;
      String bullet = String("·  ") + line.substring(2);
      lv_obj_t *l = lv_label_create(parent);
      lv_label_set_text(l, bullet.c_str());
      lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
      lv_obj_set_style_text_color(l, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_set_width(l, LV_PCT(100));
      lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
      continue;
    } else if (line.startsWith("> ")) {
      font = &moki_fraunces_italic_22;
      txt = line.c_str() + 2;
      color = MOKI_DARK;
    }

    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
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

  // Footer / build info
  lv_obj_t *foot = lv_label_create(scr);
  lv_label_set_text(foot, "MOKI · BUILD VOM 30. APRIL · v0.4");
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

void build_chat_detail(void) {
  if (g_active_chat < 0 || g_active_chat >= SAMPLE_CHATS_COUNT) {
    switch_screen(SCR_CHAT); return;
  }
  const moki_chat_t *c = &SAMPLE_CHATS[g_active_chat];
  const chat_msg_t *msgs = NULL; int mn = 0;
  if (g_active_chat == 0) { msgs = MSGS_LINA;       mn = 2; }
  else if (g_active_chat == 1) { msgs = MSGS_LESEKREIS; mn = 3; }
  else if (g_active_chat == 2) { msgs = MSGS_RHEIN;     mn = 3; }

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
  char k[48]; snprintf(k, sizeof(k), "%s · %s",
                       chat_kind_glyph(c->kind),
                       !strcmp(c->kind,"direct") ? "DIREKT" :
                       !strcmp(c->kind,"group")  ? "GRUPPE" : "ÖFFENTLICH");
  lv_label_set_text(kicker, k);
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, c->name);
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_36, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  if (c->reset && c->reset[0]) {
    lv_obj_t *r = lv_label_create(scr);
    lv_label_set_text(r, chat_reset_phrase(c->reset));
    lv_obj_set_style_text_font(r, &moki_jetbrains_mono_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(r, 2, LV_PART_MAIN);
  }

  // Messages (bubble-ish — own messages right, others left)
  for (int i = 0; i < mn; i++) {
    bool own = (msgs[i].from[0] == 'l' && !strcmp(msgs[i].from, "levin"));  // none, all are others
    (void)own;
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

// ----------------------------------------------------------------------------
// ui_entry — initial render: home screen.
// ----------------------------------------------------------------------------
static void ui_entry(void) {
  switch_screen(SCR_HOME);
}

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

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

  // Synthetic indev — host-driven via Serial commands (tap X Y / long X Y).
  static lv_indev_drv_t synth_drv;
  lv_indev_drv_init(&synth_drv);
  synth_drv.type    = LV_INDEV_TYPE_POINTER;
  synth_drv.read_cb = synth_read_cb;
  lv_indev_drv_register(&synth_drv);
  Serial.println(F("[lvgl] synth indev registered"));

  state_load_todos();
  state_load_habits();
  state_load_mood();
  state_load_notes();
  state_load_settings();
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

void loop() {
  // LVGL ticker
  lv_timer_handler();

  // Host-driven synthetic touch + diagnostic commands
  poll_serial();

  // Direct touch poll — bypasses LVGL to confirm raw hardware state.
  static bool was_pressed = false;
  bool now_pressed = touch.isPressed();
  if (now_pressed != was_pressed) {
    Serial.printf("[touch poll] state %d → %d\n", was_pressed, now_pressed);
    was_pressed = now_pressed;
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
  delay(1);
}
