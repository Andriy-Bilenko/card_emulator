/*
 * Copyright (c) 2026 Andrii Bilenko <bilenko.a.uni@gmail.com>.
 * All rights reserved.
 */

#pragma once
#include <Arduino.h>
#include <stdint.h>

// logs a formatted message to the serial port using printf-style formatting
void log(const char* fmt, ...);
// logs label and the hex dump of a buffer to the serial port
void logHex(const char* label, const uint8_t* data, size_t len);
