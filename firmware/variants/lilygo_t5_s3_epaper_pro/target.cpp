// MeshCore target glue for LILYGO T5 S3 Pro.
// Power-on of LoRa is gated by the XL9555 IO expander; the Moki firmware
// already does that in lora_power_on() before this radio_init() runs.

#include <Arduino.h>
#include "target.h"

ESP32Board board;

// Use the global Arduino SPI (FSPI on ESP32-S3) — main.cpp's lora_init()
// does SPI.begin() with our pins, so we share that bus and don't need our
// own SPIClass instance.
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);
WRAPPER_CLASS radio_driver(radio, board);

#ifndef LORA_CR
  #define LORA_CR 5
#endif

bool radio_init() {
  SPI.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
  return radio.std_init(&SPI);
}

uint32_t radio_get_rng_seed() {
  return radio.random(0x7FFFFFFF);
}

void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
  radio.setFrequency(freq);
  radio.setSpreadingFactor(sf);
  radio.setBandwidth(bw);
  radio.setCodingRate(cr);
}

void radio_set_tx_power(int8_t dbm) {
  radio.setOutputPower(dbm);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
