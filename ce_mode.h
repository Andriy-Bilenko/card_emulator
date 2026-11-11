/*
 * Copyright (c) 2026 Andrii Bilenko <bilenko.a.uni@gmail.com>.
 * All rights reserved.
 */

#pragma once

#include <stdint.h>

// device state machine

enum CeMode : uint8_t {
	CE_MODE_IDLE = 0,	// TODO: remove this state, keep only READ and WRITE
	CE_MODE_READ = 1,	// waiting/loading FIME profile
	CE_MODE_WRITE = 2,	// emulating loaded profile
};

/**
 * @brief initializes the device state machine with CE_MODE_IDLE and loads the
 * FIME profile from SPIFFS
 */
void ceModeInit();
// gets the current device mode
CeMode ceModeGet();
/**
 * @brief sets the current device mode
 * if m = WRITE - tries to load the FIME profile from SPIFFS and checks if the
 * PN532 is ready
 */
void ceModeSet(CeMode m);
/**
 * @brief Read and handle pending USB-Serial commands (up to 16 lines)
 * Must be called regularly from the main loop, if we stop reading, CDC
 * upload/mode commands sit unread and the host times out
 */
void ceModePollSerial();
/**
 * @brief Top-level work function for Arduino loop()
 * Always polls Serial first, then runs behaviour for the current mode:
 *   IDLE  — listen for commands only
 *   READ  — accept profile LOAD… upload from the host
 *   WRITE — wait for a reader (AsTarget), run one EMV session, then repeat
 */
void ceModeLoop();
