/*
 * Copyright (c) 2026 Andrii Bilenko <bilenko.a.uni@gmail.com>.
 * All rights reserved.
 */

#include "nfc.h"

#include <SPI.h>
#include <string.h>

#include "ce_profile.h"
#include "log.h"

Adafruit_PN532 nfc(PN532_SS, &SPI);
static bool gNfcOk = false;

static uint8_t gPrefetch[CE_MAX_APDU_REC];
static uint8_t gPrefetchLen = 0;
static bool gHavePrefetch = false;
static bool gAsTargetPendingLog = false;
static uint8_t gAsTargetPrefetch = 0;
static uint8_t gAsTargetIns = 0;

void led(bool on) { digitalWrite(LED_PIN, on ? LOW : HIGH); }

bool nfcOk() { return gNfcOk; }

bool nfcBegin() {
	pinMode(LED_PIN, OUTPUT);
	led(false);
	gHavePrefetch = false;
	gPrefetchLen = 0;

	pinMode(PN532_SS, OUTPUT);
	digitalWrite(PN532_SS, HIGH);
	SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
	nfc.begin();
	delay(250);

	for (uint8_t attempt = 1; attempt <= 10; attempt++) {
		uint32_t v = nfc.getFirmwareVersion();
		log("pn532 probe try=%u/%u ver=0x%08lX", attempt, 10, (unsigned long)v);
		if (v) {
			log("pn532 ok chip=%02X fw=%u.%u", (unsigned)((v >> 24) & 0xFF),
				(unsigned)((v >> 16) & 0xFF), (unsigned)((v >> 8) & 0xFF));
			gNfcOk = true;
			return true;
		}
		delay(300);
	}
	log("ERR no_pn532");
	gNfcOk = false;
	return false;
}

bool nfcRecover(const char* why) {
	log("pn532 recover (reason: %s)", why ? why : "?");
	return nfcBegin();
}

bool nfcTargetPrepare() {
	gHavePrefetch = false;
	gPrefetchLen = 0;

	if (gNfcOk) {
		uint32_t v = nfc.getFirmwareVersion();
		if (v) {
			/* Probe only — do not SAMConfig here (breaks CTOS field detect). */
			log("PN532 antenna ready");
			return true;
		}
		log("PN532 antenna NOT ready, starting recovery");
		gNfcOk = false;
	}

	delay(100);
	if (!nfcRecover("target prepare")) {
		log("ERR target prepare: no_pn532");
		return false;
	}
	log("PN532 antenna ready (recovered)");
	return true;
}

bool nfcAsTarget(uint16_t timeoutMs) {
	if (!gNfcOk) {
		log("AsTarget FAIL (pn532 not ok)");
		return false;
	}
	gHavePrefetch = false;
	gPrefetchLen = 0;

	/*
	 * workingTrx1: AsTarget() default 30s. Keep one continuous arm — no 5s
	 * re-issue churn. ATQA/frame live in Adafruit AsTarget (0x04,0x00).
	 */
	uint16_t t = timeoutMs ? timeoutMs : (uint16_t)30000;
	led(true);
	log("AsTarget listening (ATQA 0400) up to %u ms — present NOW",
		(unsigned)t);
	bool ok = nfc.AsTarget(t);
	led(false);
	if (!ok) {
		(void)nfc.SAMConfig();
		log("AsTarget FAIL (no activation within %u ms)", (unsigned)t);
		return false;
	}

	uint8_t len = 0;
	memset(gPrefetch, 0, sizeof(gPrefetch));
	if (nfc.getDataTarget(gPrefetch, &len, 2000) && len > 0) {
		if (!(len >= 1 && gPrefetch[0] == 0xE0)) {
			gPrefetchLen = len;
			gHavePrefetch = true;
		}
	}
	gAsTargetPrefetch = gHavePrefetch ? 1 : 0;
	gAsTargetIns =
		(gHavePrefetch && gPrefetchLen >= 2) ? gPrefetch[1] : (uint8_t)0;
	gAsTargetPendingLog = true;
	log("AsTarget OK, activated prefetch=%u INS=%02X", gAsTargetPrefetch,
		gAsTargetIns);
	return true;
}

void nfcLogAsTargetDeferred() {
	if (!gAsTargetPendingLog) return;
	gAsTargetPendingLog = false;
	log("AsTarget OK, activated prefetch=%u INS=%02X", gAsTargetPrefetch,
		gAsTargetIns);
}

bool nfcGetData(uint8_t* cmd, uint8_t* cmdLen) {
	if (gHavePrefetch) {
		memcpy(cmd, gPrefetch, gPrefetchLen);
		*cmdLen = gPrefetchLen;
		gHavePrefetch = false;
		gPrefetchLen = 0;
		return true;
	}
	return nfc.getDataTarget(cmd, cmdLen, 5000);
}

bool nfcSetData(const uint8_t* apdu, size_t apduLen) {
	if (!apdu || apduLen == 0 || apduLen > CE_MAX_APDU_REC) {
		if (apduLen > CE_MAX_APDU_REC)
			log("TgSetData too long len=%u max=%u", (unsigned)apduLen,
				(unsigned)CE_MAX_APDU_REC);
		return false;
	}

	const size_t kSingleMax = 253;
	const size_t kChunk = 128;
	uint8_t frame[1 + 253];

	if (apduLen <= kSingleMax) {
		frame[0] = 0x8E;
		memcpy(frame + 1, apdu, apduLen);
		uint8_t flen = (uint8_t)(apduLen + 1);
		bool ok = false;
		for (uint8_t t = 0; t < 5; t++) {
			if (nfc.setDataTarget(frame, flen)) {
				ok = true;
				break;
			}
		}
		if (!ok) {
			log("TgSetData FAIL len=%u cmd=8E", (unsigned)apduLen);
			return false;
		}
		log("TgSetData ok len=%u", (unsigned)apduLen);
		return true;
	}

	size_t off = 0;
	while (off < apduLen) {
		size_t n = apduLen - off;
		if (n > kChunk) n = kChunk;
		bool last = (off + n >= apduLen);
		frame[0] = last ? 0x8E : 0x94;
		memcpy(frame + 1, apdu + off, n);
		uint8_t flen = (uint8_t)(n + 1);
		bool ok = false;
		for (uint8_t t = 0; t < 5; t++) {
			if (nfc.setDataTarget(frame, flen)) {
				ok = true;
				break;
			}
		}
		if (!ok) {
			log("TgSetData FAIL len=%u off=%u cmd=%02X", (unsigned)apduLen,
				(unsigned)off, frame[0]);
			return false;
		}
		off += n;
	}
	log("TgSetData ok len=%u (chained)", (unsigned)apduLen);
	return true;
}

void nfcEndTarget() {
	gHavePrefetch = false;
	gPrefetchLen = 0;
	nfcLogAsTargetDeferred();
	/*
	 * Soft re-arm: probe + SAMConfig. Full nfcBegin() between taps creates a
	 * dead window where CTOS only sees PENDING.
	 */
	uint32_t v = nfc.getFirmwareVersion();
	if (!v) {
		gNfcOk = false;
		if (!nfcRecover("post-session"))
			log("ERR post-session recover failed");
		return;
	}
	if (!nfc.SAMConfig())
		log("WARN post-session SAMConfig failed");
	delay(30);
}
