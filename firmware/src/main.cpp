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
static lv_obj_t *build_dock(lv_obj_t *parent) {
  static const char *items[] = {"heim", "tun", "lesen", "chat", "karte"};
  const int active_idx = 0;                  // home active

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
    lv_obj_add_event_cb(item, on_element_tapped,
                        LV_EVENT_CLICKED, (void *)items[i]);

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
// ui_entry — Stage 2a: home screen skeleton (status bar, content, dock).
// ----------------------------------------------------------------------------
static void ui_entry(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  build_status_bar(scr);
  build_home_content(scr);
  build_dock(scr);
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
