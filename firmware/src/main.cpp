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
typedef struct { const char *name; uint8_t today_count; uint8_t streak; } moki_habit_t;
typedef struct {
  const char *title;
  const char *cat;        // home/plants/work/self/social
  const char *deadline;   // heute/morgen/diese woche/28. apr/...
  bool recurring;         // weekly
  bool done;
} moki_todo_t;
typedef struct {
  uint8_t day; const char *hour; const char *title; const char *place;
  const char *kind;       // private/friends/public
} moki_event_t;

static const moki_habit_t SAMPLE_HABITS[] = {
  { "Lesen · 10 min", 1, 4 },
  { "Spaziergang",    2, 7 },
  { "Wasser",         3, 2 },
  { "Tagebuch",       0, 0 },
};
static const int SAMPLE_HABITS_COUNT = sizeof(SAMPLE_HABITS) / sizeof(SAMPLE_HABITS[0]);

static const moki_todo_t SAMPLE_TODOS[] = {
  { "Pflanzen gießen",    "plants", "morgen",      true,  false },
  { "Lina zurückrufen",   "social", "heute",       false, false },
  { "Steuerkram sortieren","work",  "diese woche", false, false },
  { "zum arzt",           "self",   "28. apr",     false, false },
  { "Küche wischen",      "home",   "",            true,  true  },
};
static const int SAMPLE_TODOS_COUNT = sizeof(SAMPLE_TODOS) / sizeof(SAMPLE_TODOS[0]);

static const moki_event_t SAMPLE_EVENTS[] = {
  { 2, "19:00", "kochen mit lina",            "zuhause",     "private" },
  { 3, "10:00", "spaziergang philosophenweg", "heidelberg",  "friends" },
  { 5, "20:30", "lesekreis · walden",         "café frieda", "public"  },
};
static const int SAMPLE_EVENTS_COUNT = sizeof(SAMPLE_EVENTS) / sizeof(SAMPLE_EVENTS[0]);

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
  if (!strcmp(cat,"home"))   return "◯";
  if (!strcmp(cat,"plants")) return "◐";
  if (!strcmp(cat,"work"))   return "◑";
  if (!strcmp(cat,"self"))   return "◉";
  if (!strcmp(cat,"social")) return "◈";
  return "·";
}

// ============================================================================
// Screen IDs + nav
// ============================================================================
typedef enum { SCR_HOME = 0, SCR_DO, SCR_READ, SCR_CHAT, SCR_MAP } screen_id_t;
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

// LVGL indev — read touch state from GT911. GT911 native is panel-landscape
// (960×540); our display rotation is INVERTED_PORTRAIT (540×960). The mapping
// is a 270° rotation between native and rotated coordinate frames.
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int16_t tx[5], ty[5];
  if (touch.isPressed()) {
    uint8_t n = touch.getPoint(tx, ty, 1);
    if (n > 0) {
      int16_t raw_x = tx[0];
      int16_t raw_y = ty[0];
      data->state   = LV_INDEV_STATE_PRESSED;
      data->point.x = raw_y;                 // landscape→portrait swap
      data->point.y = 960 - raw_x;
      return;
    }
  }
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

  // ---- Paws (ellipses cx=40,60 cy=86 rx=6 ry=3.5, fill=INK) ----
  lv_canvas_draw_rect(cv,  68, 165, 24, 14, &body);
  lv_canvas_draw_rect(cv, 108, 165, 24, 14, &body);

  return cv;
}

// ----------------------------------------------------------------------------
// build_status_bar — top strip: sync indicator left, battery + time right,
// dashed bottom border per design DNA.
// ----------------------------------------------------------------------------
static lv_obj_t *build_status_bar(lv_obj_t *parent) {
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LV_PCT(100), 38);
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
  lv_obj_set_style_text_font(sync, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sync, 2, LV_PART_MAIN);
  lv_obj_add_flag(sync, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(sync, on_element_tapped, LV_EVENT_CLICKED, (void *)"sync");

  lv_obj_t *right = lv_label_create(bar);
  lv_label_set_text(right, "78  14:32");
  lv_obj_set_style_text_color(right, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_font(right, &moki_jetbrains_mono_18, LV_PART_MAIN);
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
  lv_obj_set_size(dock, LV_PCT(100), 60);
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
    lv_obj_set_size(item, 90, 50);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(item, on_dock_clicked,
                        LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *lbl = lv_label_create(item);
    lv_label_set_text(lbl, items[i]);
    lv_obj_set_style_text_font(lbl, &moki_jetbrains_mono_22, LV_PART_MAIN);
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
  lv_obj_set_size(tile, LV_PCT(31), 90);
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
  lv_obj_set_style_text_font(k, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(k, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(k, 2, LV_PART_MAIN);

  lv_obj_t *v = lv_label_create(tile);
  lv_label_set_text(v, value);
  lv_obj_set_style_text_font(v, &moki_fraunces_regular_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(v, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  lv_obj_t *s = lv_label_create(tile);
  lv_label_set_text(s, sub);
  lv_obj_set_style_text_font(s, &moki_jetbrains_mono_18, LV_PART_MAIN);
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

  // -- Date kicker --
  lv_obj_t *kicker = lv_label_create(col);
  lv_label_set_text(kicker, "DIENSTAG · 20. APRIL");
  lv_obj_set_style_text_font(kicker, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(kicker, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(kicker, 3, LV_PART_MAIN);

  // -- Title (italic-ish, will become Fraunces in 2c) --
  lv_obj_t *title = lv_label_create(col);
  lv_label_set_text(title, "langsam, aber jeden tag.");
  lv_obj_set_style_text_font(title, &moki_fraunces_italic_28, LV_PART_MAIN);
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
  lv_obj_set_style_text_font(pet_name, &moki_fraunces_regular_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(pet_name, lv_color_hex(MOKI_INK), LV_PART_MAIN);

  lv_obj_t *pet_meta = lv_label_create(pet_pair);
  lv_label_set_text(pet_meta, "TAG 14 · 3 IN FOLGE");
  lv_obj_set_style_text_font(pet_meta, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(pet_meta, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(pet_meta, 2, LV_PART_MAIN);

  // -- Mood pill (dashed-style border, full-width) --
  lv_obj_t *mood = lv_obj_create(col);
  lv_obj_remove_style_all(mood);
  lv_obj_set_size(mood, LV_PCT(100), 60);
  lv_obj_set_style_bg_color(mood, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(mood, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(mood, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_border_width(mood, 1, LV_PART_MAIN);
  // LVGL doesn't ship dashed borders; 1px solid LIGHT is the closest stylistic
  // match for the dashed look until we draw a custom border pattern.
  lv_obj_set_style_radius(mood, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_left(mood, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_right(mood, 14, LV_PART_MAIN);
  lv_obj_set_flex_flow(mood, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(mood, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(mood, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(mood, on_element_tapped, LV_EVENT_CLICKED, (void *)"mood");

  lv_obj_t *mq = lv_label_create(mood);
  lv_label_set_text(mq, "wie fühlst du dich heute?");
  lv_obj_set_style_text_font(mq, &moki_fraunces_italic_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(mq, lv_color_hex(MOKI_DARK), LV_PART_MAIN);

  lv_obj_t *ma = lv_label_create(mood);
  lv_label_set_text(ma, "TEILEN →");
  lv_obj_set_style_text_font(ma, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(ma, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(ma, 2, LV_PART_MAIN);

  // -- Three stat tiles --
  lv_obj_t *tiles = lv_obj_create(col);
  lv_obj_remove_style_all(tiles);
  lv_obj_set_size(tiles, LV_PCT(100), 95);
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
    case SCR_HOME: build_home();  break;
    case SCR_DO:   build_do();    break;
    case SCR_READ: build_read();  break;
    case SCR_CHAT: build_chats(); break;
    case SCR_MAP:  build_map();   break;
  }
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
  lv_obj_set_height(btn, 40);
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
  lv_obj_set_style_text_font(lbl, &moki_jetbrains_mono_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl,
      lv_color_hex(active ? MOKI_PAPER : MOKI_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lbl, 2, LV_PART_MAIN);
}

static lv_obj_t *make_tab_bar(lv_obj_t *parent) {
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LV_PCT(100), 46);
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
static void build_todo_row(lv_obj_t *parent, const moki_todo_t *t) {
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

  // Checkbox
  lv_obj_t *cb = lv_obj_create(row);
  lv_obj_remove_style_all(cb);
  lv_obj_set_size(cb, 22, 22);
  lv_obj_set_style_border_color(cb, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_border_width(cb, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(cb, lv_color_hex(t->done ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(cb, LV_OPA_COVER, LV_PART_MAIN);
  if (t->done) {
    lv_obj_t *check = lv_label_create(cb);
    lv_label_set_text(check, "✓");
    lv_obj_set_style_text_color(check, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
    lv_obj_set_style_text_font(check, &moki_fraunces_regular_28, LV_PART_MAIN);
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
  lv_obj_set_style_text_font(title, &moki_fraunces_regular_28, LV_PART_MAIN);
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
  lv_obj_set_style_text_font(m, &moki_jetbrains_mono_18, LV_PART_MAIN);
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
    // Open
    for (int i = 0; i < SAMPLE_TODOS_COUNT; i++) {
      if (!SAMPLE_TODOS[i].done) build_todo_row(body, &SAMPLE_TODOS[i]);
    }
    for (int i = 0; i < SAMPLE_TODOS_COUNT; i++) {
      if (SAMPLE_TODOS[i].done) {
        if (!printed_done_header) {
          lv_obj_t *hdr = lv_label_create(body);
          lv_label_set_text(hdr, "ERLEDIGT");
          lv_obj_set_style_text_font(hdr, &moki_jetbrains_mono_18, LV_PART_MAIN);
          lv_obj_set_style_text_color(hdr, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
          lv_obj_set_style_text_letter_space(hdr, 3, LV_PART_MAIN);
          lv_obj_set_style_pad_top(hdr, 12, LV_PART_MAIN);
          lv_obj_set_style_pad_bottom(hdr, 4, LV_PART_MAIN);
          printed_done_header = true;
        }
        build_todo_row(body, &SAMPLE_TODOS[i]);
      }
    }
  } else if (current_do_tab == DO_HABITS) {
    for (int i = 0; i < SAMPLE_HABITS_COUNT; i++) {
      const moki_habit_t *h = &SAMPLE_HABITS[i];
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

      lv_obj_t *txt = lv_obj_create(row);
      lv_obj_remove_style_all(txt);
      lv_obj_set_flex_grow(txt, 1);
      lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);

      lv_obj_t *name = lv_label_create(txt);
      lv_label_set_text(name, h->name);
      lv_obj_set_style_text_font(name, &moki_fraunces_regular_28, LV_PART_MAIN);
      lv_obj_set_style_text_color(name, lv_color_hex(MOKI_INK), LV_PART_MAIN);

      char ser[32];
      snprintf(ser, sizeof(ser), "%s",
               h->streak > 0 ? "•••○○" : "noch keine serie");
      // Render streak as filled/empty dots — quick approximation
      char dots[16] = {0};
      int filled = h->streak > 5 ? 5 : (int)h->streak;
      for (int d = 0; d < 5; d++) {
        strcat(dots, d < filled ? "● " : "○ ");
      }
      lv_obj_t *s = lv_label_create(txt);
      lv_label_set_text(s, dots);
      lv_obj_set_style_text_font(s, &moki_jetbrains_mono_18, LV_PART_MAIN);
      lv_obj_set_style_text_color(s, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
      lv_obj_set_style_pad_top(s, 4, LV_PART_MAIN);

      // Count badge
      lv_obj_t *pill = lv_obj_create(row);
      lv_obj_remove_style_all(pill);
      lv_obj_set_size(pill, 70, 44);
      lv_obj_set_style_bg_color(pill,
        lv_color_hex(h->today_count > 0 ? MOKI_INK : MOKI_PAPER), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(pill, lv_color_hex(MOKI_INK), LV_PART_MAIN);
      lv_obj_set_style_border_width(pill, 1, LV_PART_MAIN);
      lv_obj_set_style_radius(pill, 2, LV_PART_MAIN);
      lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

      char cnt[8];
      snprintf(cnt, sizeof(cnt), "%u×", (unsigned)h->today_count);
      lv_obj_t *cl = lv_label_create(pill);
      lv_label_set_text(cl, cnt);
      lv_obj_set_style_text_font(cl, &moki_fraunces_regular_28, LV_PART_MAIN);
      lv_obj_set_style_text_color(cl,
        lv_color_hex(h->today_count > 0 ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
    }
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
      lv_obj_set_style_text_font(w, &moki_jetbrains_mono_18, LV_PART_MAIN);
      lv_obj_set_style_text_color(w,
        lv_color_hex(active ? MOKI_LIGHT : MOKI_DARK), LV_PART_MAIN);

      lv_obj_t *d = lv_label_create(day);
      lv_label_set_text(d, dates[i]);
      lv_obj_set_style_text_font(d, &moki_fraunces_regular_28, LV_PART_MAIN);
      lv_obj_set_style_text_color(d,
        lv_color_hex(active ? MOKI_PAPER : MOKI_INK), LV_PART_MAIN);
    }

    // Upcoming events
    lv_obj_t *hdr = lv_label_create(body);
    lv_label_set_text(hdr, "KOMMEND");
    lv_obj_set_style_text_font(hdr, &moki_jetbrains_mono_18, LV_PART_MAIN);
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
      lv_obj_set_style_text_font(date, &moki_jetbrains_mono_18, LV_PART_MAIN);
      lv_obj_set_style_text_color(date, lv_color_hex(MOKI_INK), LV_PART_MAIN);

      lv_obj_t *col2 = lv_obj_create(row);
      lv_obj_remove_style_all(col2);
      lv_obj_set_flex_grow(col2, 1);
      lv_obj_set_flex_flow(col2, LV_FLEX_FLOW_COLUMN);

      lv_obj_t *t = lv_label_create(col2);
      lv_label_set_text(t, ev->title);
      lv_obj_set_style_text_font(t, &moki_fraunces_regular_28, LV_PART_MAIN);
      lv_obj_set_style_text_color(t, lv_color_hex(MOKI_INK), LV_PART_MAIN);

      char place_kind[64];
      const char *vis = !strcmp(ev->kind,"public") ? "öffentlich"
                       : !strcmp(ev->kind,"friends") ? "freund_innen"
                       : "privat";
      snprintf(place_kind, sizeof(place_kind), "%s · %s", ev->place, vis);
      lv_obj_t *p = lv_label_create(col2);
      lv_label_set_text(p, place_kind);
      lv_obj_set_style_text_font(p, &moki_jetbrains_mono_18, LV_PART_MAIN);
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
    lv_obj_set_style_text_font(author, &moki_jetbrains_mono_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(author, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(author, 3, LV_PART_MAIN);
    lv_obj_set_style_text_align(author, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(author, LV_PCT(100));

    lv_obj_t *book = lv_label_create(col);
    lv_label_set_text(book, "Walden");
    lv_obj_set_style_text_font(book, &moki_fraunces_italic_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(book, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_align(book, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(book, LV_PCT(100));

    lv_obj_t *excerpt = lv_label_create(col);
    lv_label_set_text(excerpt,
      "Ich ging in die Wälder, weil ich mit Bedacht leben wollte, "
      "um nur den wesentlichen Tatsachen des Lebens ins Auge zu sehen, "
      "und zu lernen, was es zu lehren hatte — und nicht, wenn es zum "
      "Sterben käme, zu entdecken, dass ich nicht gelebt hatte.");
    lv_obj_set_style_text_font(excerpt, &moki_fraunces_italic_18, LV_PART_MAIN);
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
    lv_obj_set_style_text_font(prev, &moki_jetbrains_mono_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(prev, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(prev, 2, LV_PART_MAIN);

    lv_obj_t *page = lv_label_create(nav);
    lv_label_set_text(page, "42 / 312");
    lv_obj_set_style_text_font(page, &moki_jetbrains_mono_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(page, lv_color_hex(MOKI_INK), LV_PART_MAIN);

    lv_obj_t *next = lv_label_create(nav);
    lv_label_set_text(next, "WEITER →");
    lv_obj_set_style_text_font(next, &moki_jetbrains_mono_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(next, lv_color_hex(MOKI_INK), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(next, 2, LV_PART_MAIN);
  } else {
    // FEED + NOTES stubs
    lv_obj_t *stub = lv_label_create(col);
    lv_label_set_text(stub, current_read_tab == READ_FEED
                              ? "feed kommt bald."
                              : "notizen kommen bald.");
    lv_obj_set_style_text_font(stub, &moki_fraunces_italic_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(stub, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
    lv_obj_set_style_text_align(stub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(stub, LV_PCT(100));
  }
}

static void build_read(void) {
  lv_obj_t *content = build_screen_chrome(lv_scr_act(), 2);
  build_read_content(content);
}

// ----------------------------------------------------------------------------
// CHATS (stub — list comes in next iteration)
// ----------------------------------------------------------------------------
static void build_chats(void) {
  lv_obj_t *content = build_screen_chrome(lv_scr_act(), 3);
  lv_obj_t *l = lv_label_create(content);
  lv_label_set_text(l, "chats kommen bald.");
  lv_obj_set_style_text_font(l, &moki_fraunces_italic_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_center(l);
}

// ----------------------------------------------------------------------------
// MAP (stub — full stylized cartography next iteration)
// ----------------------------------------------------------------------------
static void build_map(void) {
  lv_obj_t *content = build_screen_chrome(lv_scr_act(), 4);
  lv_obj_t *l = lv_label_create(content);
  lv_label_set_text(l, "karte kommt bald.");
  lv_obj_set_style_text_font(l, &moki_fraunces_italic_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(MOKI_DARK), LV_PART_MAIN);
  lv_obj_center(l);
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

  Serial.println(F("[lvgl] ui_entry"));
  ui_entry();
}

void loop() {
  // LVGL ticker
  lv_timer_handler();

  // Heartbeat once per second so we can correlate with the screen.
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
