// MeshCore variant for LILYGO T5 E-Paper S3 Pro (custom Moki firmware).
// Based on lilygo_t3s3 variant pattern. Pin map matches firmware/src/main.cpp.
//
// We DON'T use MeshCore's AutoDiscoverRTCClock here — Moki has its own
// PCF85063 integration via SensorLib. RTC integration into MeshCore happens
// later through a thin adapter (TODO).

#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/ESP32Board.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

extern ESP32Board board;
extern WRAPPER_CLASS radio_driver;

bool radio_init();
uint32_t radio_get_rng_seed();
void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
void radio_set_tx_power(int8_t dbm);
mesh::LocalIdentity radio_new_identity();
