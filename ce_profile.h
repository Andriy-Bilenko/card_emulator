#pragma once

#include <Arduino.h>
#include <stdint.h>

#define CE_PROF_MAGIC 0x35503654u
#define CE_PROF_VERSION 5
#define CE_MAX_AID 16
#define CE_MAX_FCI 200
#define CE_MAX_GPO 80
#define CE_MAX_APPS 4
#define CE_MAX_SHARED_REC 8
#define CE_MAX_APP_REC 16
#define CE_MAX_REC 512
#define CE_MAX_PAN 10
#define CE_MAX_CDOL 48
#define CE_MAX_ICC 192
#define CE_MAX_IAD 32
#define CE_MAX_BLOB 8192
#define CE_MAX_RECORDS_TOTAL 32

#define CE_FLAG_HAS_IMK 0x01
#define CE_FLAG_HAS_LIVE_AC 0x02
#define CE_FLAG_HAS_PSE 0x04
#define CE_FLAG_HAS_ICC 0x08
#define CE_FLAG_HAS_IAD 0x10
#define CE_FLAG_PPSE_DERIVED 0x20
#define CE_FLAG_HAS_IDN 0x40
#define CE_FLAG_VALID 0x80

#define CE_SEL_NONE 0
#define CE_SEL_PPSE 1
#define CE_SEL_PSE 2
#define CE_SEL_APP 3

// one EMV file record
struct CeRecord {
	uint8_t sfi;
	uint8_t rec;
	uint16_t len;
	uint8_t data[CE_MAX_REC];
};

// one payment application
struct CeApp {
	uint8_t flags;
	uint8_t aidLen;
	uint8_t aid[CE_MAX_AID];
	uint8_t fciLen;
	uint8_t fci[CE_MAX_FCI];
	uint8_t gpoLen;
	uint8_t gpo[CE_MAX_GPO];
	uint8_t numRecords;
	CeRecord records[CE_MAX_APP_REC];
	uint8_t panLen;
	uint8_t pan[CE_MAX_PAN];
	uint8_t psn;
	uint8_t imkAc[16];
	uint8_t mkIdn[16];
	uint8_t cdol1Len;
	uint8_t cdol1[CE_MAX_CDOL];
	uint8_t cdol2Len;
	uint8_t cdol2[CE_MAX_CDOL];
	uint8_t iadLen;
	uint8_t iad[CE_MAX_IAD];
	uint16_t atc;
	uint8_t pinTry;
	uint8_t iccNLen;
	uint8_t iccN[CE_MAX_ICC];
	uint8_t iccDLen;
	uint8_t iccD[CE_MAX_ICC];
	uint8_t iccELen;
	uint8_t iccE[3];
};

// one bank card
struct CeProfile {
	uint8_t version;
	uint8_t flags;
	uint8_t ppseLen;
	uint8_t ppse[CE_MAX_FCI];
	uint8_t pseLen;
	uint8_t pse[CE_MAX_FCI];
	uint8_t numShared;
	CeRecord shared[CE_MAX_SHARED_REC];
	uint8_t numApps;
	CeApp apps[CE_MAX_APPS];
	uint8_t sel;
	uint8_t curApp;
};

// zeroes all fields of the profile
void ceProfileClear(CeProfile* p);
// unpacks a profile from a binary blob
bool ceProfileUnpack(const uint8_t* blob, size_t len, CeProfile* p);
// checks if a profile is valid
bool ceProfileIsValid(const CeProfile* p);
// saves a profile to the SPIFFS filesystem
bool ceProfileSave(const CeProfile* p);
// loads a profile from the SPIFFS filesystem
bool ceProfileLoad(CeProfile* p);
// persists the ATC (application transaction counter) to the SPIFFS filesystem
bool ceProfilePersistAtc(const CeProfile* p);
// logs a summary of the profile
void ceProfileLogSummary(const CeProfile* p);
// returns the current application (selected application) or nullptr
CeApp* ceProfileCurApp(CeProfile* p);

extern CeProfile gProfile;
