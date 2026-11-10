#include "nfc.h"

#include <SPI.h>
#include <string.h>

#include "log.h"

Adafruit_PN532 nfc(PN532_SS, &SPI);
static bool gNfcOk = false;

static uint8_t gPrefetch[APDU_MAX];
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
	if (!gNfcOk) return false;
	gHavePrefetch = false;
	gPrefetchLen = 0;

	bool ok = timeoutMs ? nfc.AsTarget(timeoutMs) : nfc.AsTarget();
	if (!ok) return false;

	/* Prefetch first C-APDU */
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
	if (!apdu || apduLen == 0 || apduLen > APDU_MAX) return false;
	if (apduLen > PN532_SETDATA_MAX) {
		log("TgSetData too long len=%u max=%u (PN532 PICC)", (unsigned)apduLen,
			(unsigned)PN532_SETDATA_MAX);
		return false;
	}

	uint8_t frame[1 + PN532_SETDATA_MAX];
	frame[0] = 0x8E;
	memcpy(frame + 1, apdu, apduLen);
	uint16_t flen = (uint16_t)(apduLen + 1);

	bool ok = false;
	for (uint8_t t = 0; t < 5; t++) {
		if (nfc.setDataTarget(frame, flen)) {
			ok = true;
			break;
		}
	}
	if (!ok) {
		log("TgSetData FAIL len=%u", (unsigned)apduLen);
		return false;
	}
	log("TgSetData ok len=%u", (unsigned)apduLen);
	return true;
}

void nfcEndTarget() {
	gHavePrefetch = false;
	gPrefetchLen = 0;
	nfcLogAsTargetDeferred();
}
