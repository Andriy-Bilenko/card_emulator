#include "ce_mode.h"

#include <string.h>

#include "ce_emulate.h"
#include "ce_profile.h"
#include "log.h"
#include "nfc.h"

/* configurer.py waits for these substrings*/
#define CE_MSG_LOAD_ACK "LOAD ack"
#define CE_MSG_PROFILE_OK "profile_ok"

static CeMode gMode = CE_MODE_IDLE;
static uint32_t gWriteWarnMs = 0;

static uint8_t gLoadBuf[CE_MAX_BLOB];
static size_t gLoadLen = 0;
static size_t gLoadExpect = 0;
static bool gLoading = false;

void ceModeInit() {
	gMode = CE_MODE_IDLE;
	if (ceProfileLoad(&gProfile))
		log("STATE mode=IDLE profile=loaded");
	else
		log("STATE mode=IDLE profile=empty");
	log("ready");
}

CeMode ceModeGet() { return gMode; }

void ceModeSet(CeMode m) {
	if (m == CE_MODE_WRITE && gMode == CE_MODE_WRITE && nfcOk()) {
		log("STATE mode=WRITE - already armed");
		return;
	}

	gMode = m;
	if (m == CE_MODE_READ) {
		log("STATE mode=READ - send LOAD BEGIN/…/END with FIME profile");
	} else if (m == CE_MODE_WRITE) {
		if (!ceProfileIsValid(&gProfile)) ceProfileLoad(&gProfile);
		if (!nfcTargetPrepare()) {
			gMode = CE_MODE_IDLE;
			log("ERR WRITE not armed - PN532 not initialised");
			return;
		}
		log("STATE mode=WRITE - emulate FIME profile");
		if (!ceProfileIsValid(&gProfile))
			log("WARN no profile - run configurer.py read first");
		else
			ceProfileLogSummary(&gProfile);
	} else {
		log("STATE mode=IDLE");
	}
}

/**
 * @brief converts a hexadecimal character to int
 * char "F" -> int 15
 */
static int hexNibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/**
 * @brief parses a string of hexadecimal bytes into a byte array
 * string "12 34 56 78" -> byte array [0x12, 0x34, 0x56, 0x78]
 *
 * @param s the string to parse
 * @param out the output byte array
 * @param maxOut the maximum number of bytes to output
 * @param nOut the number of bytes output
 * @return true if the string was parsed successfully, false otherwise
 */
static bool parseHexBytes(const char* s, uint8_t* out, size_t maxOut,
						  size_t* nOut) {
	size_t n = 0;
	while (*s) {
		while (*s == ' ') s++;
		if (!*s) break;
		int hi = hexNibble(*s++);
		if (hi < 0 || !*s) return false;
		int lo = hexNibble(*s++);
		if (lo < 0) return false;
		if (n >= maxOut) return false;
		out[n++] = (uint8_t)((hi << 4) | lo);
	}
	*nOut = n;
	return true;
}

/**
 * @brief Handles one profile-upload line from the host
 *
 * Expected ASCII lines, case-sensitive "LOAD" prefix:
 *   LOAD BEGIN <decimal_byte_count>
 *   LOAD <hex_offset> <hex_bytes...>   e.g. LOAD 0 A4040C...
 *   ...
 *   LOAD END
 * Chunks are written into gLoadBuf until END is reached
 * when getting to END, unpacks and saves to SPIFFS
 * Each successful BEGIN/chunk is answered with CE_MSG_LOAD_ACK on Serial
 *
 * @param cmd Already-trimmed line that begins with "LOAD"
 */
static void handleLoadLine(const char* cmd) {
	if (strncmp(cmd, "LOAD BEGIN ", 11) == 0) {
		gLoadExpect = (size_t)atoi(cmd + 11);
		gLoadLen = 0;
		gLoading = true;
		if (gLoadExpect == 0 || gLoadExpect > CE_MAX_BLOB) {
			log("ERR load bad size");
			gLoading = false;
			return;
		}
		log("LOAD begin expect=%u", (unsigned)gLoadExpect);
		log("%s", CE_MSG_LOAD_ACK);
		return;
	}
	if (strncmp(cmd, "LOAD END", 8) == 0) {
		if (!gLoading || gLoadLen != gLoadExpect) {
			log("ERR load incomplete have=%u expect=%u", (unsigned)gLoadLen,
				(unsigned)gLoadExpect);
			gLoading = false;
			return;
		}
		if (!ceProfileUnpack(gLoadBuf, gLoadLen, &gProfile)) {
			log("ERR load unpack failed");
			gLoading = false;
			return;
		}
		if (!ceProfileSave(&gProfile)) {
			log("ERR load save failed");
			gLoading = false;
			return;
		}
		gLoading = false;
		log("STATE %s bytes=%u apps=%u", CE_MSG_PROFILE_OK, (unsigned)gLoadLen,
			gProfile.numApps);
		ceProfileLogSummary(&gProfile);
		// Don't touch PN532 here: past bug was re-init/SPI during USB CDC LOAD,
		// which restricted SPI and logged false no_pn532, WRITE prepare probes
		// later
		return;
	}
	if (strncmp(cmd, "LOAD ", 5) == 0) {
		if (!gLoading) {
			log("ERR load not started");
			return;
		}
		const char* p = cmd + 5;
		char offHex[8];
		size_t oi = 0;
		while (*p && *p != ' ' && oi + 1 < sizeof(offHex)) offHex[oi++] = *p++;
		offHex[oi] = 0;
		while (*p == ' ') p++;
		unsigned off = 0;
		for (size_t i = 0; offHex[i]; i++) {
			int v = hexNibble(offHex[i]);
			if (v < 0) {
				log("ERR load bad offset");
				return;
			}
			off = (off << 4) | (unsigned)v;
		}
		uint8_t chunk[64];
		size_t n = 0;
		if (!parseHexBytes(p, chunk, sizeof(chunk), &n)) {
			log("ERR load bad hex");
			return;
		}
		if (off + n > gLoadExpect || off + n > CE_MAX_BLOB) {
			log("ERR load overflow");
			return;
		}
		memcpy(gLoadBuf + off, chunk, n);
		if (off + n > gLoadLen) gLoadLen = off + n;
		log("%s", CE_MSG_LOAD_ACK);
		return;
	}
	log("ERR unknown_cmd");
}

/**
 * @brief Reads one Serial text line and dispatches it
 *
 * Line format: ASCII ended by \n or \r\n, max 219 chars
 *   LOAD...          - handleLoadLine (profile upload)
 *   MODE READ|WRITE  - ceModeSet (case-insensitive after uppercasing)
 *
 * @return true  = finished a full line (even if it was only whitespace)
 *         false = nothing waiting on Serial, or line not finished yet
 */
static bool ceModePollSerialOnce() {
	if (!Serial.available()) return false;

	char line[220];
	size_t n = 0;
	bool sawEnd = false;
	uint32_t t0 = millis();
	while (n + 1 < sizeof(line)) {
		if (!Serial.available()) {
			if (millis() - t0 > 50) break;
			delay(1);
			continue;
		}
		char c = (char)Serial.read();
		t0 = millis();
		if (c == '\n' || c == '\r') {
			sawEnd = true;
			if (c == '\r' && Serial.available()) {
				int p = Serial.peek();
				if (p == '\n') Serial.read();
			}
			break;
		}
		line[n++] = c;
	}
	line[n] = 0;

	if (!sawEnd) {
		if (n + 1 >= sizeof(line)) {
			log("ERR load line overflow");
			while (Serial.available()) {
				char c = (char)Serial.read();
				if (c == '\n' || c == '\r') break;
			}
		}
		return false;
	}

	while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
	size_t start = 0;
	while (line[start] == ' ' || line[start] == '\t') start++;
	if (!line[start]) return true;

	if (strncmp(line + start, "LOAD", 4) == 0) {
		handleLoadLine(line + start);
		return true;
	}

	for (size_t i = start; line[i]; i++) {
		if (line[i] >= 'a' && line[i] <= 'z')
			line[i] = (char)(line[i] - 'a' + 'A');
	}
	const char* cmd = line + start;
	log("CMD %s", cmd);

	if (strcmp(cmd, "MODE READ") == 0)
		ceModeSet(CE_MODE_READ);
	else if (strcmp(cmd, "MODE WRITE") == 0)
		ceModeSet(CE_MODE_WRITE);
	else
		log("ERR unknown_cmd");
	return true;
}

void ceModePollSerial() {
	for (uint8_t i = 0; i < 16; i++) {
		if (!ceModePollSerialOnce()) break;
	}
}

void ceModeLoop() {
	ceModePollSerial();

	if (gMode == CE_MODE_READ) {
		if (gLoading || Serial.available()) return;
		delay(5);
		return;
	}

	if (gMode == CE_MODE_WRITE) {
		if (!ceProfileIsValid(&gProfile)) {
			uint32_t now = millis();
			if (now - gWriteWarnMs > 3000) {
				gWriteWarnMs = now;
				log("WARN WRITE: no profile - configurer.py read first");
			}
			delay(50);
			return;
		}
		if (!nfcOk()) {
			log("ERR WRITE blocked - PN532 not initialised");
			delay(1000);
			return;
		}
		ceModePollSerial();
		if (gMode != CE_MODE_WRITE) return;
		if (!nfcAsTarget(0)) return;
		ceEmulateSession(&gProfile);
		nfcEndTarget();
		delay(200);
	}
}
