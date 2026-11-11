/*
 * Copyright (c) 2026 Andrii Bilenko <bilenko.a.uni@gmail.com>.
 * All rights reserved.
 */

#pragma once

#include <Adafruit_PN532.h>
#include <Arduino.h>
#include <stdint.h>

#define PN532_SCK 4
#define PN532_MISO 5
#define PN532_MOSI 6
#define PN532_SS 7
#define LED_PIN 8

// BY ME TODO: add comments to all functions, AI fucking removed them all
void led(bool on);
bool nfcBegin();
bool nfcOk();
bool nfcRecover(const char* why);
bool nfcTargetPrepare();
bool nfcAsTarget(uint16_t timeoutMs = 0);
bool nfcGetData(uint8_t* cmd, uint8_t* cmdLen);
bool nfcSetData(const uint8_t* apdu, size_t apduLen);
void nfcLogAsTargetDeferred();
void nfcEndTarget();
