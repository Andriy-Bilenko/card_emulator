/*
 * Copyright (c) 2026 Andrii Bilenko <bilenko.a.uni@gmail.com>.
 * All rights reserved.
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void log(const char* fmt, ...) {
	char buf[220];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Serial.print(F("LOG "));
	Serial.println(buf);
}

void logHex(const char* label, const uint8_t* data, size_t len) {
	Serial.print(F("LOG "));
	Serial.print(label);
	Serial.print(F(" len="));
	Serial.print(len);
	Serial.print(F(" hex="));
	for (size_t i = 0; i < len; i++) {
		if (data[i] < 0x10) Serial.print('0');
		Serial.print(data[i], HEX);
	}
	Serial.println();
}
