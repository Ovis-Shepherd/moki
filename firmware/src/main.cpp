// ============================================================================
// MOKI · firmware · Stage 0
// ============================================================================
// Goal: prove the toolchain. Boot the T5, talk to the Mac over native USB-CDC,
// blink "moki alive" once per second so we can confirm flashing works.
//
// Once we see this in the serial monitor, Stage 0 is done and we move on to
// the display + LVGL + GT911 in Stage 1.
// ============================================================================

#include <Arduino.h>

void setup() {
  Serial.begin(115200);

  // Native USB-CDC enumerates after boot — wait briefly so the first lines
  // are not lost when the host attaches the monitor right after flashing.
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) {
    delay(10);
  }

  Serial.println();
  Serial.println(F("--------------------------------------------------"));
  Serial.println(F("moki · stage 0 · hello, hardware"));
  Serial.printf ("  ESP-IDF       : %s\n",  ESP.getSdkVersion());
  Serial.printf ("  Chip          : %s rev %d\n",
                 ESP.getChipModel(), (int)ESP.getChipRevision());
  Serial.printf ("  PSRAM (free)  : %u bytes\n", (unsigned)ESP.getFreePsram());
  Serial.printf ("  Flash size    : %u bytes\n", (unsigned)ESP.getFlashChipSize());
  Serial.println(F("--------------------------------------------------"));
}

void loop() {
  static uint32_t tick = 0;
  Serial.printf("moki alive · t=%lus · heap=%u\n",
                (unsigned long)(millis() / 1000),
                (unsigned)ESP.getFreeHeap());
  tick++;
  delay(1000);
}
