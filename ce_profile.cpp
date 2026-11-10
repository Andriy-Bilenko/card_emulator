#include "ce_profile.h"

#include <FS.h>
#include <SPIFFS.h>
#include <string.h>

#include "log.h"

CeProfile gProfile;

static const char* kPath = "/t6.bin";

/**
 * @brief takes n bytes from the input buffer
 *
 * @param p pointer to the input buffer
 * @param left pointer to the remaining length of the input buffer
 * @param dst pointer to the destination buffer
 * @param n number of bytes to take
 * @return true if the bytes were taken successfully
 * @return false if the bytes were not taken successfully
 */
static bool take(const uint8_t** p, size_t* left, void* dst, size_t n) {
	if (*left < n) return false;
	memcpy(dst, *p, n);
	*p += n;
	*left -= n;
	return true;
}

/**
 * @brief takes a blob from the input buffer
 *
 * @param p pointer to the input buffer
 * @param left pointer to the remaining length of the input buffer
 * @param dst pointer to the destination buffer
 * @param dstLen pointer to the destination length
 * @param maxLen maximum length of the blob
 * @return true if the blob was taken successfully
 * @return false if the blob was not taken successfully
 */
static bool takeBlob(const uint8_t** p, size_t* left, uint8_t* dst,
					 uint8_t* dstLen, uint8_t maxLen) {
	uint8_t L = 0;
	if (!take(p, left, &L, 1) || L > maxLen || !take(p, left, dst, L))
		return false;
	*dstLen = L;
	return true;
}

/**
 * @brief takes a record from the input buffer
 *
 * @param p pointer to the input buffer
 * @param left pointer to the remaining length of the input buffer
 * @param recs pointer to the destination buffer
 * @param nOut pointer to the destination length
 * @param maxN maximum number of records
 * @return true if the records were taken successfully, else false
 */
static bool takeRecords(const uint8_t** p, size_t* left, CeRecord* recs,
						uint8_t* nOut, uint8_t maxN) {
	uint8_t n = 0;
	if (!take(p, left, &n, 1) || n > maxN) return false;
	*nOut = n;
	for (uint8_t i = 0; i < n; i++) {
		uint8_t sfi, rec, hi, lo;
		if (!take(p, left, &sfi, 1) || !take(p, left, &rec, 1) ||
			!take(p, left, &hi, 1) || !take(p, left, &lo, 1))
			return false;
		uint16_t L = (uint16_t)((hi << 8) | lo);
		if (L > CE_MAX_REC || !take(p, left, recs[i].data, L)) return false;
		recs[i].sfi = sfi;
		recs[i].rec = rec;
		recs[i].len = L;
	}
	return true;
}

/**
 * @brief packs a blob into the output buffer
 *
 * @param blob pointer to the output buffer
 * @param n pointer to the current position in the output buffer
 * @param cap maximum capacity of the output buffer
 * @param d pointer to the data to pack
 * @param L length of the data to pack
 * @return true if the blob was packed successfully, else false
 */
static bool packPutBlob(uint8_t* blob, size_t* n, size_t cap, const uint8_t* d,
						uint8_t L) {
	if (*n + 1 + L > cap) return false;
	blob[(*n)++] = L;
	memcpy(blob + *n, d, L);
	*n += L;
	return true;
}

/**
 * @brief packs multiple records into the output buffer
 *
 * @param blob pointer to the output buffer
 * @param n pointer to the current position in the output buffer
 * @param cap maximum capacity of the output buffer in bytes
 * @param r pointer to the records to pack
 * @param nr number of records to pack
 * @return true if the records were packed successfully, else false
 */
static bool packPutRecs(uint8_t* blob, size_t* n, size_t cap, const CeRecord* r,
						uint8_t nr) {
	if (*n + 1 > cap) return false;
	blob[(*n)++] = nr;
	for (uint8_t i = 0; i < nr; i++) {
		if (*n + 4 + r[i].len > cap) return false;
		blob[(*n)++] = r[i].sfi;
		blob[(*n)++] = r[i].rec;
		blob[(*n)++] = (uint8_t)(r[i].len >> 8);
		blob[(*n)++] = (uint8_t)(r[i].len & 0xFF);
		memcpy(blob + *n, r[i].data, r[i].len);
		*n += r[i].len;
	}
	return true;
}

// ensures the SPIFFS filesystem is mounted
static bool ensureFs() {
	static bool ok = false;
	if (ok) return true;
	if (!SPIFFS.begin(true)) {
		log("ERR spiffs begin");
		return false;
	}
	ok = true;
	return true;
}

/**
 * @brief finds a tag in the EMV data
 *
 * @param p pointer to the EMV data
 * @param n length of the EMV data
 * @param tag the tag to find
 * @param outLen pointer to the length of the tag
 * @return pointer to the tag if it was found, else nullptr
 */
static const uint8_t* findTag(const uint8_t* p, size_t n, uint16_t tag,
							  size_t* outLen) {
	size_t i = 0;
	while (i < n) {
		if (p[i] == 0x00 || p[i] == 0xFF) {
			i++;
			continue;
		}
		uint16_t t;
		size_t j = i;
		if ((p[j] & 0x1F) == 0x1F) {
			t = p[j++];
			while (j < n && (p[j] & 0x80)) t = (uint16_t)((t << 8) | p[j++]);
			if (j >= n) return nullptr;
			t = (uint16_t)((t << 8) | p[j++]);
		} else {
			t = p[j++];
		}
		if (j >= n) return nullptr;
		size_t ln;
		if (p[j] & 0x80) {
			uint8_t nb = (uint8_t)(p[j++] & 0x7F);
			if (j + nb > n) return nullptr;
			ln = 0;
			for (uint8_t k = 0; k < nb; k++) ln = (ln << 8) | p[j++];
		} else {
			ln = p[j++];
		}
		if (j + ln > n) return nullptr;
		if (t == tag) {
			*outLen = ln;
			return p + j;
		}
		if (t & 0x20) {
			const uint8_t* hit = findTag(p + j, ln, tag, outLen);
			if (hit) return hit;
		}
		j += ln;
		i = j;
	}
	return nullptr;
}

void ceProfileClear(CeProfile* p) { memset(p, 0, sizeof(*p)); }

bool ceProfileIsValid(const CeProfile* p) {
	return (p->flags & CE_FLAG_VALID) && p->numApps > 0 && p->ppseLen > 0 &&
		   p->apps[0].aidLen && p->apps[0].gpoLen && p->apps[0].fciLen;
}

CeApp* ceProfileCurApp(CeProfile* p) {
	if (p->sel != CE_SEL_APP || p->curApp >= p->numApps) return nullptr;
	return &p->apps[p->curApp];
}

bool ceProfileUnpack(const uint8_t* blob, size_t len, CeProfile* out) {
	ceProfileClear(out);
	if (len < 8 || len > CE_MAX_BLOB) return false;
	uint32_t magic = (uint32_t)blob[0] | ((uint32_t)blob[1] << 8) |
					 ((uint32_t)blob[2] << 16) | ((uint32_t)blob[3] << 24);
	if (magic != CE_PROF_MAGIC) {
		log("ERR profile magic want T6P5");
		return false;
	}
	const uint8_t* p = blob + 4;
	size_t left = len - 4;
	uint8_t ver = 0, flags = 0, nApps = 0;
	if (!take(&p, &left, &ver, 1) || !take(&p, &left, &flags, 1)) return false;
	if (ver != CE_PROF_VERSION) {
		log("ERR profile ver=%u want=%u", ver, CE_PROF_VERSION);
		return false;
	}
	out->version = ver;
	out->flags = flags;

	if (!takeBlob(&p, &left, out->ppse, &out->ppseLen, CE_MAX_FCI))
		return false;
	if (!takeBlob(&p, &left, out->pse, &out->pseLen, CE_MAX_FCI)) return false;
	if (!takeRecords(&p, &left, out->shared, &out->numShared,
					 CE_MAX_SHARED_REC))
		return false;

	if (!take(&p, &left, &nApps, 1) || nApps == 0 || nApps > CE_MAX_APPS)
		return false;
	out->numApps = nApps;

	uint16_t totalRec = out->numShared;
	for (uint8_t i = 0; i < nApps; i++) {
		CeApp* a = &out->apps[i];
		if (!take(&p, &left, &a->flags, 1)) return false;
		if (!takeBlob(&p, &left, a->aid, &a->aidLen, CE_MAX_AID)) return false;
		if (!takeBlob(&p, &left, a->fci, &a->fciLen, CE_MAX_FCI)) return false;
		if (!takeBlob(&p, &left, a->gpo, &a->gpoLen, CE_MAX_GPO)) return false;
		if (!takeRecords(&p, &left, a->records, &a->numRecords, CE_MAX_APP_REC))
			return false;
		totalRec = (uint16_t)(totalRec + a->numRecords);
		if (!takeBlob(&p, &left, a->pan, &a->panLen, CE_MAX_PAN)) return false;
		if (!take(&p, &left, &a->psn, 1)) return false;
		if (!take(&p, &left, a->imkAc, 16)) return false;
		if (!take(&p, &left, a->mkIdn, 16)) return false;
		if (!takeBlob(&p, &left, a->cdol1, &a->cdol1Len, CE_MAX_CDOL))
			return false;
		if (!takeBlob(&p, &left, a->cdol2, &a->cdol2Len, CE_MAX_CDOL))
			return false;
		if (!takeBlob(&p, &left, a->iad, &a->iadLen, CE_MAX_IAD)) return false;
		if (a->iadLen)
			a->flags |= CE_FLAG_HAS_IAD;
		else
			a->flags = (uint8_t)(a->flags & ~CE_FLAG_HAS_IAD);
		uint8_t atcHi = 0, atcLo = 0;
		if (!take(&p, &left, &atcHi, 1) || !take(&p, &left, &atcLo, 1))
			return false;
		a->atc = (uint16_t)((atcHi << 8) | atcLo);
		if (!take(&p, &left, &a->pinTry, 1)) return false;
		if (!takeBlob(&p, &left, a->iccN, &a->iccNLen, CE_MAX_ICC))
			return false;
		if (!takeBlob(&p, &left, a->iccD, &a->iccDLen, CE_MAX_ICC))
			return false;
		if (!takeBlob(&p, &left, a->iccE, &a->iccELen, 3)) return false;
		if (a->iccNLen && a->iccDLen && a->iccNLen == a->iccDLen && a->iccELen)
			a->flags |= CE_FLAG_HAS_ICC;
		else {
			a->flags = (uint8_t)(a->flags & ~CE_FLAG_HAS_ICC);
			a->iccNLen = a->iccDLen = a->iccELen = 0;
		}
	}
	if (totalRec > CE_MAX_RECORDS_TOTAL) return false;

	out->flags |= CE_FLAG_VALID;
	out->sel = CE_SEL_NONE;
	out->curApp = 0;
	return true;
}

bool ceProfileSave(const CeProfile* p) {
	if (!ceProfileIsValid(p) || !ensureFs()) return false;

	static uint8_t blob[CE_MAX_BLOB];
	size_t n = 0;
	blob[n++] = (uint8_t)(CE_PROF_MAGIC & 0xFF);
	blob[n++] = (uint8_t)((CE_PROF_MAGIC >> 8) & 0xFF);
	blob[n++] = (uint8_t)((CE_PROF_MAGIC >> 16) & 0xFF);
	blob[n++] = (uint8_t)((CE_PROF_MAGIC >> 24) & 0xFF);
	blob[n++] = CE_PROF_VERSION;
	blob[n++] = (uint8_t)(p->flags & ~CE_FLAG_VALID);

	if (!packPutBlob(blob, &n, sizeof(blob), p->ppse, p->ppseLen) ||
		!packPutBlob(blob, &n, sizeof(blob), p->pse, p->pseLen) ||
		!packPutRecs(blob, &n, sizeof(blob), p->shared, p->numShared))
		return false;
	if (n + 1 > sizeof(blob)) return false;
	blob[n++] = p->numApps;
	for (uint8_t i = 0; i < p->numApps; i++) {
		const CeApp* a = &p->apps[i];
		if (n + 1 > sizeof(blob)) return false;
		blob[n++] = a->flags;
		if (!packPutBlob(blob, &n, sizeof(blob), a->aid, a->aidLen) ||
			!packPutBlob(blob, &n, sizeof(blob), a->fci, a->fciLen) ||
			!packPutBlob(blob, &n, sizeof(blob), a->gpo, a->gpoLen) ||
			!packPutRecs(blob, &n, sizeof(blob), a->records, a->numRecords) ||
			!packPutBlob(blob, &n, sizeof(blob), a->pan, a->panLen))
			return false;
		if (n + 1 + 16 + 16 + 1 + a->cdol1Len + 1 + a->cdol2Len + 1 +
				a->iadLen + 3 >
			sizeof(blob))
			return false;
		blob[n++] = a->psn;
		memcpy(blob + n, a->imkAc, 16);
		n += 16;
		memcpy(blob + n, a->mkIdn, 16);
		n += 16;
		blob[n++] = a->cdol1Len;
		memcpy(blob + n, a->cdol1, a->cdol1Len);
		n += a->cdol1Len;
		blob[n++] = a->cdol2Len;
		memcpy(blob + n, a->cdol2, a->cdol2Len);
		n += a->cdol2Len;
		blob[n++] = a->iadLen;
		memcpy(blob + n, a->iad, a->iadLen);
		n += a->iadLen;
		blob[n++] = (uint8_t)(a->atc >> 8);
		blob[n++] = (uint8_t)(a->atc & 0xFF);
		blob[n++] = a->pinTry;
		if ((a->flags & CE_FLAG_HAS_ICC) && a->iccNLen && a->iccDLen) {
			if (!packPutBlob(blob, &n, sizeof(blob), a->iccN, a->iccNLen) ||
				!packPutBlob(blob, &n, sizeof(blob), a->iccD, a->iccDLen) ||
				!packPutBlob(blob, &n, sizeof(blob), a->iccE, a->iccELen))
				return false;
		} else {
			if (n + 3 > sizeof(blob)) return false;
			blob[n++] = 0;
			blob[n++] = 0;
			blob[n++] = 0;
		}
	}

	File f = SPIFFS.open(kPath, FILE_WRITE);
	if (!f) return false;
	size_t w = f.write(blob, n);
	f.close();
	if (w != n) return false;
	log("profile saved apps=%u bytes=%u", p->numApps, (unsigned)n);
	return true;
}

bool ceProfileLoad(CeProfile* p) {
	ceProfileClear(p);
	if (!ensureFs() || !SPIFFS.exists(kPath)) return false;
	File f = SPIFFS.open(kPath, FILE_READ);
	if (!f) return false;
	static uint8_t blob[CE_MAX_BLOB];
	size_t n = f.read(blob, sizeof(blob));
	f.close();
	if (n == 0) return false;
	return ceProfileUnpack(blob, n, p);
}

bool ceProfilePersistAtc(const CeProfile* p) { return ceProfileSave(p); }

void ceProfileLogSummary(const CeProfile* p) {
	log("profile apps=%u ppse=%u shared=%u flags=0x%02X ppse_derived=%u",
		p->numApps, p->ppseLen, p->numShared, p->flags,
		(p->flags & CE_FLAG_PPSE_DERIVED) ? 1 : 0);
	for (uint8_t i = 0; i < p->numApps; i++) {
		const CeApp* a = &p->apps[i];
		log("  app%u aidLen=%u gpo=%u rec=%u atc=%u icc=%u iad=%u imk=%u "
			"idn=%u",
			i, a->aidLen, a->gpoLen, a->numRecords, a->atc,
			(a->flags & CE_FLAG_HAS_ICC) ? 1 : 0,
			(a->flags & CE_FLAG_HAS_IAD) ? 1 : 0,
			(a->flags & CE_FLAG_HAS_IMK) ? 1 : 0,
			(a->flags & CE_FLAG_HAS_IDN) ? 1 : 0);
		logHex("  AID", a->aid, a->aidLen);
		/* Fingerprint issuer (90) / ICC (9F46) certs so host can confirm LOAD*/
		for (uint8_t r = 0; r < a->numRecords; r++) {
			size_t ln = 0;
			const uint8_t* v =
				findTag(a->records[r].data, a->records[r].len, 0x90, &ln);
			if (v && ln >= 4)
				log("  SFI%u/%u 90=%02X%02X%02X%02X len=%u", a->records[r].sfi,
					a->records[r].rec, v[0], v[1], v[2], v[3], (unsigned)ln);
			v = findTag(a->records[r].data, a->records[r].len, 0x9F46, &ln);
			if (v && ln >= 4)
				log("  SFI%u/%u 9F46=%02X%02X%02X%02X len=%u",
					a->records[r].sfi, a->records[r].rec, v[0], v[1], v[2],
					v[3], (unsigned)ln);
		}
	}
}
