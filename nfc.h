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

/**
 * @brief Drive the status LED (active-low on this board)
 * @param on true = lit, false = off
 */
void led(bool on);

/**
 * @brief Init SPI + PN532, probe firmware version until ready or give up
 * @return true if the chip answered getFirmwareVersion
 */
bool nfcBegin();

/**
 * @brief Whether the last nfcBegin/nfcRecover left the PN532 usable
 */
bool nfcOk();

/**
 * @brief Re-run nfcBegin after a fault (logs why)
 * @param why short reason string for the log line
 * @return true if the chip is usable again
 */
bool nfcRecover(const char* why);

/**
 * @brief Probe the PN532 before MODE WRITE, recover if the last probe failed
 * @return true if antenna/chip is ready for emulating a card
 */
bool nfcTargetPrepare();

/**
 * @brief puts PN532 into "be a card" mode and waits for a reader to activate
 * Blocks in TgInitAsTarget until a reader activates (or timeout)
 *
 *
 * Prefetches the first C-APDU via TgGetData when present (skips RATS 0xE0)
 * @param timeoutMs 0 = library default (~30s), else pass-through to AsTarget.
 *        Timeout must cover waitready after ACK (patched Adafruit); without
 *        waitready the PICC never stays armed and POS sees only PENDING.
 * @return true if a reader activated the target
 */
bool nfcAsTarget(uint16_t timeoutMs = 0);

/**
 * @brief Read next ISO-DEP C-APDU (prefetch buffer or TgGetData)
 * @param cmd    output buffer (at least CE_MAX_APDU_REC)
 * @param cmdLen in/out length
 * @return true if a non-empty APDU was copied
 */
bool nfcGetData(uint8_t* cmd, uint8_t* cmdLen);

/**
 * @brief Send one R-APDU (TgSetData, chains via TgSetMetaData if >253)
 * @param apdu    response bytes including SW
 * @param apduLen length in bytes (≤ CE_MAX_APDU_REC)
 * @return true if PN532 accepted the frame(s)
 */
bool nfcSetData(const uint8_t* apdu, size_t apduLen);

/**
 * @brief Prints the “AsTarget OK…” log line later, not during the RF exchange
 * (USB printing mid-tap can break timing)
 */
void nfcLogAsTargetDeferred();

/**
 * @brief Session cleanup: drop any stashed first APDU and print that deferred
 * OK line if it was still pending
 */
void nfcEndTarget();
