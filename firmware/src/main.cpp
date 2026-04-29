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
static lv_obj_t     *moki_label = NULL;
static bool          tipping     = false;

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

// LVGL flush callback — converts 32-bit color to 8-bit greyscale into our
// decode buffer; a periodic timer (flush_timer_cb) then rotates that buffer
// into the epdiy framebuffer and triggers a partial refresh.
static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  if (disp_flush_enabled) {
    uint16_t       w   = lv_area_get_width(area);
    uint16_t       h   = lv_area_get_height(area);
    lv_color32_t  *t32 = (lv_color32_t *)color_p;

    for (int i = 0; i < (w * h); i++) {
      lv_color8_t ret;
      LV_COLOR_SET_R8(ret, LV_COLOR_GET_R(*t32) >> 5);
      LV_COLOR_SET_G8(ret, LV_COLOR_GET_G(*t32) >> 5);
      LV_COLOR_SET_B8(ret, LV_COLOR_GET_B(*t32) >> 6);
      decodebuffer[i] = ret.full;
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
  decodebuffer    = (uint8_t   *)ps_calloc(sizeof(uint8_t),    DISP_BUF_SIZE);

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
// below assumes 270° rotation between native and rotated coordinate frames.
// For Stage 1c the screen-level click handler doesn't depend on exact coords.
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

// Screen-level click handler — toggles the label text on every tap.
static void on_screen_clicked(lv_event_t *e) {
  tipping = !tipping;
  lv_label_set_text(moki_label, tipping ? "moki tippt" : "moki alive");
  Serial.printf("[touch] tap · label now: \"%s\"\n",
                tipping ? "moki tippt" : "moki alive");
}

// ----------------------------------------------------------------------------
// ui_entry — Stage 1c: clickable PAPER screen, INK label that toggles on tap.
// Stage 2 will replace this with the real home screen.
// ----------------------------------------------------------------------------
static void ui_entry(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(MOKI_PAPER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, on_screen_clicked, LV_EVENT_CLICKED, NULL);

  moki_label = lv_label_create(scr);
  lv_label_set_text(moki_label, "moki alive");
  lv_obj_set_style_text_color(moki_label, lv_color_hex(MOKI_INK), LV_PART_MAIN);
  lv_obj_set_style_text_font(moki_label, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(moki_label, LV_ALIGN_CENTER, 0, 0);
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

  // Clear once on boot — wipes whatever was last persisted on the panel.
  epd_poweron();
  epd_clear();
  temperature = epd_ambient_temperature();
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
