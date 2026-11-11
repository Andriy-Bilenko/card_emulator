/*
 * Copyright (c) 2026 Andrii Bilenko <bilenko.a.uni@gmail.com>.
 * All rights reserved.
 */

#include "ce_mode.h"
#include "log.h"
#include "nfc.h"

void setup() {
	Serial.begin(115200);
	delay(800);
	log("card emulator boot");
	if (!nfcBegin()) {
		while (true) {
			log("ERR PN532 init failed, retry after reboot");
			delay(2000);
		}
	}
	ceModeInit();
}

void loop() { ceModeLoop(); }
