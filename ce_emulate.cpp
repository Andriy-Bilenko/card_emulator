#include "ce_emulate.h"

// BY ME TODO: comments to static functions, read/understand/cleanup everything
// here

#include <string.h>

#include "ce_crypto.h"
#include "ce_icc.h"
#include "ce_mode.h"
#include "log.h"
#include "nfc.h"

static const uint8_t SW_6A82[] = {0x6A, 0x82};
static const uint8_t SW_6A83[] = {0x6A, 0x83};
static const uint8_t SW_6A88[] = {0x6A, 0x88};
static const uint8_t SW_6985[] = {0x69, 0x85};
static const uint8_t SW_6D00[] = {0x6D, 0x00};

static const uint8_t PPSE_NAME[] = {'2', 'P', 'A', 'Y', '.', 'S', 'Y',
									'S', '.', 'D', 'D', 'F', '0', '1'};
static const uint8_t PSE_NAME[] = {'1', 'P', 'A', 'Y', '.', 'S', 'Y',
								   'S', '.', 'D', 'D', 'F', '0', '1'};

static uint8_t gPdol[64];
static uint8_t gPdolLen = 0;

/* Deferred session log - never Serial.print on the FWT hot path */
enum { EVT_RX = 1, EVT_TX, EVT_NOTE, EVT_ERR };
struct CeEvt {
	uint8_t kind;
	uint8_t ins;
	uint8_t p1;
	uint8_t sw1;
	uint8_t sw2;
	uint16_t len;
	uint8_t ok;
	uint8_t note; /* 1=PPSE 2=AID 3=PSE 4=GPO 5=READ 6=GENAC 7=DDA 8=omitIAD */
	uint8_t sfi;  /* READ only */
	uint8_t rec;  /* READ only */
};
static CeEvt gEvt[24];
static uint8_t gEvtN = 0;

static void evtPush(uint8_t kind, uint8_t ins, uint8_t p1, uint16_t len,
					uint8_t sw1, uint8_t sw2, uint8_t ok, uint8_t note,
					uint8_t sfi = 0, uint8_t rec = 0) {
	if (gEvtN >= sizeof(gEvt) / sizeof(gEvt[0])) return;
	CeEvt* e = &gEvt[gEvtN++];
	e->kind = kind;
	e->ins = ins;
	e->p1 = p1;
	e->len = len;
	e->sw1 = sw1;
	e->sw2 = sw2;
	e->ok = ok;
	e->note = note;
	e->sfi = sfi;
	e->rec = rec;
}

static bool selectData(const uint8_t* cmd, uint8_t cmdLen, const uint8_t** data,
					   uint8_t* lc) {
	if (cmdLen < 5) return false;
	*lc = cmd[4];
	if (cmdLen < (uint8_t)(5 + *lc)) return false;
	*data = cmd + 5;
	return true;
}

static bool reply(const uint8_t* apdu, size_t len) {
	return nfcSetData(apdu, len);
}

static uint8_t gInsLog[12];
static uint8_t gOkLog[12];
static uint8_t gLogN = 0;
static bool gHadGen = false;
static bool gGenLive = false;
static bool gHadCda = false;
static bool gHadDda = false;
static uint16_t gGenAtc = 0;
static bool gAtcDirty = false;
static bool gOmitIad = false;
uint8_t gTdhDbg[20];
uint16_t gTdhInLen = 0;
uint8_t gTdhPdolLen = 0;

static void noteApdu(uint8_t ins, bool ok) {
	if (gLogN < sizeof(gInsLog)) {
		gInsLog[gLogN] = ins;
		gOkLog[gLogN] = ok ? 1 : 0;
		gLogN++;
	}
}

static bool findRecord(const CeRecord* recs, uint8_t n, uint8_t sfi,
					   uint8_t rec, const uint8_t** data, uint16_t* len) {
	for (uint8_t i = 0; i < n; i++) {
		if (recs[i].sfi == sfi && recs[i].rec == rec) {
			*data = recs[i].data;
			*len = recs[i].len;
			return true;
		}
	}
	return false;
}

static size_t putTlv(uint8_t* dst, size_t cap, size_t o, uint16_t tag,
					 const uint8_t* val, uint8_t vlen) {
	if (tag > 0xFF) {
		if (o + 2 + 1 + vlen > cap) return cap + 1;
		dst[o++] = (uint8_t)(tag >> 8);
		dst[o++] = (uint8_t)(tag & 0xFF);
	} else {
		if (o + 1 + 1 + vlen > cap) return cap + 1;
		dst[o++] = (uint8_t)tag;
	}
	dst[o++] = vlen;
	memcpy(dst + o, val, vlen);
	return o + vlen;
}

static bool buildGenAc(CeApp* a, const uint8_t* cmd, uint8_t cmdLen,
					   uint8_t* out, uint8_t* outLen, bool* liveOut,
					   bool* cdaOut, uint16_t* atcOut) {
	uint8_t p1 = cmdLen >= 3 ? cmd[2] : 0;
	uint8_t req = p1 & 0xC0;
	bool wantCda = (p1 & 0x10) != 0;
	uint8_t cid = req;
	if (cid != 0x80 && cid != 0x40 && cid != 0x00) cid = 0x80;

	uint8_t lc = cmdLen >= 5 ? cmd[4] : 0;
	const uint8_t* cdolData = (cmdLen >= 5 + lc) ? cmd + 5 : nullptr;
	size_t need = cdol1_data_len(a);
	if (!cdolData || lc < 8) return false;
	size_t macLen = (lc >= need && need > 0) ? need : lc;

	if (!(a->flags & CE_FLAG_HAS_IMK) || !(a->flags & CE_FLAG_HAS_LIVE_AC) ||
		!a->panLen)
		return false;

	uint8_t ac[8];
	uint16_t atc = a->atc ? a->atc : 1;
	if (!compute_ac(a, atc, cdolData, macLen, ac)) return false;

	/* CID is AC type only (00/40/80). Do NOT OR P1 CDA bit 0x10 into CID -
	 * that sets TVR/reason bits and breaks Kernel 2 CAM on FEIG. CDA is
	 * indicated by presence of 9F4B, not by CID bit 5. */

	uint8_t un4[4];
	cdol1_un(a, cdolData, (uint8_t)macLen, un4);

	uint8_t prefix[96];
	size_t po = 0;
	uint8_t atcB[2] = {(uint8_t)(atc >> 8), (uint8_t)(atc & 0xFF)};
	bool putAc = !wantCda || !(a->flags & CE_FLAG_HAS_ICC);
	po = putTlv(prefix, sizeof(prefix), po, 0x9F27, &cid, 1);
	po = putTlv(prefix, sizeof(prefix), po, 0x9F36, atcB, 2);
	if (putAc) po = putTlv(prefix, sizeof(prefix), po, 0x9F26, ac, 8);
	if ((a->flags & CE_FLAG_HAS_IAD) && a->iadLen > 0)
		po = putTlv(prefix, sizeof(prefix), po, 0x9F10, a->iad, a->iadLen);
	else
		gOmitIad = true;
	if (po > sizeof(prefix)) return false;

	/*
	 * CDA TDH (FEIG libemvco_c2 Step 12): PDOL‖CDOL1‖GEN-AC clear TLVs
	 * (tags+lens+values), not values-only. With CDA, 9F26 is omitted from the
	 * R-APDU so prefix is 9F27‖9F36‖9F10. Values-only CID‖ATC‖IAD was wrong
	 * for this kernel (payload len 64 vs FEIG 73).
	 */
	uint8_t cdaSig[CE_MAX_ICC];
	size_t cdaLen = sizeof(cdaSig);
	bool haveCda = false;
	if (wantCda && (a->flags & CE_FLAG_HAS_ICC)) {
		haveCda = cda_sign(a, cid, atc, ac, un4, gPdol, gPdolLen, cdolData,
						   macLen, prefix, po, cdaSig, &cdaLen);
		if (!haveCda) return false;
	} else if (wantCda) {
		return false;
	}

	uint8_t body[240];
	size_t n = 0;
	memcpy(body, prefix, po);
	n = po;
	if (haveCda) {
		if (n + 4 + cdaLen > sizeof(body)) return false;
		body[n++] = 0x9F;
		body[n++] = 0x4B;
		if (cdaLen < 128) {
			body[n++] = (uint8_t)cdaLen;
		} else {
			body[n++] = 0x81;
			body[n++] = (uint8_t)cdaLen;
		}
		memcpy(body + n, cdaSig, cdaLen);
		n += cdaLen;
	}

	size_t o = 0;
	out[o++] = 0x77;
	if (n < 128) {
		out[o++] = (uint8_t)n;
	} else {
		out[o++] = 0x81;
		out[o++] = (uint8_t)n;
	}
	if (o + n + 2 > CE_MAX_APDU_REC) return false;
	memcpy(out + o, body, n);
	o += n;
	out[o++] = 0x90;
	out[o++] = 0x00;
	*outLen = (uint8_t)o;
	*liveOut = true;
	*cdaOut = haveCda;
	*atcOut = atc;
	return true;
}

static bool handleInternalAuth(CeApp* a, const uint8_t* cmd, uint8_t cmdLen) {
	uint8_t lc = cmdLen >= 5 ? cmd[4] : 0;
	if (!lc || cmdLen < 5 + lc) return reply(SW_6985, sizeof(SW_6985));
	if (!(a->flags & CE_FLAG_HAS_ICC)) return reply(SW_6A88, sizeof(SW_6A88));

	uint16_t atc = a->atc ? a->atc : 1;
	uint8_t sig[CE_MAX_ICC];
	size_t sigLen = sizeof(sig);
	if (!dda_sign(a, atc, cmd + 5, lc, sig, &sigLen))
		return reply(SW_6985, sizeof(SW_6985));

	uint8_t resp[CE_MAX_APDU_REC];
	size_t o = 0;
	resp[o++] = 0x77;
	size_t inner = 0;
	uint8_t tmp[CE_MAX_ICC + 8];
	tmp[inner++] = 0x9F;
	tmp[inner++] = 0x4B;
	if (sigLen < 128) {
		tmp[inner++] = (uint8_t)sigLen;
	} else {
		tmp[inner++] = 0x81;
		tmp[inner++] = (uint8_t)sigLen;
	}
	memcpy(tmp + inner, sig, sigLen);
	inner += sigLen;

	if (inner < 128) {
		resp[o++] = (uint8_t)inner;
	} else {
		resp[o++] = 0x81;
		resp[o++] = (uint8_t)inner;
	}
	if (o + inner + 2 > sizeof(resp)) return reply(SW_6985, sizeof(SW_6985));
	memcpy(resp + o, tmp, inner);
	o += inner;
	resp[o++] = 0x90;
	resp[o++] = 0x00;
	gHadDda = true;
	return reply(resp, o);
}

static bool handleApdu(CeProfile* p, const uint8_t* cmd, uint8_t cmdLen) {
	if (cmdLen < 4) return reply(SW_6D00, sizeof(SW_6D00));

	const uint8_t ins = cmd[1];
	const uint8_t p1 = cmd[2];
	const uint8_t p2 = cmd[3];
	evtPush(EVT_RX, ins, p1, cmdLen, 0, 0, 1, 0);

	bool ok = false;
	bool genLive = false;
	bool genCda = false;
	uint16_t genAtc = 0;
	uint8_t note = 0;
	uint8_t readSfi = 0;
	uint8_t readRec = 0;
	const uint8_t* tx = nullptr;
	size_t txLen = 0;
	uint8_t localBuf[CE_MAX_APDU_REC];
	uint8_t localLen = 0;

	if (ins == 0xA4 && p1 == 0x04) {
		const uint8_t* data = nullptr;
		uint8_t lc = 0;
		if (!selectData(cmd, cmdLen, &data, &lc)) {
			tx = SW_6A82;
			txLen = sizeof(SW_6A82);
			ok = reply(tx, txLen);
		} else if (lc == sizeof(PPSE_NAME) &&
				   memcmp(data, PPSE_NAME, lc) == 0) {
			p->sel = CE_SEL_PPSE;
			note = 1;
			if (p->ppseLen) {
				tx = p->ppse;
				txLen = p->ppseLen;
				ok = reply(tx, txLen);
			} else {
				tx = SW_6A82;
				txLen = sizeof(SW_6A82);
				ok = reply(tx, txLen);
			}
		} else if (lc == sizeof(PSE_NAME) && memcmp(data, PSE_NAME, lc) == 0) {
			p->sel = CE_SEL_PSE;
			note = 3;
			if ((p->flags & CE_FLAG_HAS_PSE) && p->pseLen) {
				tx = p->pse;
				txLen = p->pseLen;
				ok = reply(tx, txLen);
			} else {
				tx = SW_6A82;
				txLen = sizeof(SW_6A82);
				ok = reply(tx, txLen);
			}
		} else {
			bool matched = false;
			ok = false;
			for (uint8_t i = 0; i < p->numApps; i++) {
				CeApp* a = &p->apps[i];
				if (a->aidLen && lc >= a->aidLen &&
					memcmp(data, a->aid, a->aidLen) == 0) {
					matched = true;
					p->sel = CE_SEL_APP;
					p->curApp = i;
					note = 2;
					tx = a->fci;
					txLen = a->fciLen;
					ok = reply(tx, txLen);
					break;
				}
			}
			if (!matched) {
				tx = SW_6A82;
				txLen = sizeof(SW_6A82);
				ok = reply(tx, txLen);
			}
		}
	} else if (ins == 0xA8) {
		CeApp* a = ceProfileCurApp(p);
		uint8_t lc = cmdLen >= 5 ? cmd[4] : 0;
		/* CDA TDH uses PDOL *related data* = value of tag 83 only.
		 * Empty/missing PDOL → length 0 (do NOT hash the 83 00 wrapper). */
		gPdolLen = 0;
		if (lc && cmdLen >= 5 + lc) {
			const uint8_t* d = cmd + 5;
			if (lc >= 2 && d[0] == 0x83) {
				uint8_t vl = d[1];
				if (vl < 0x80 && (uint16_t)(2 + vl) <= lc &&
					vl <= sizeof(gPdol)) {
					memcpy(gPdol, d + 2, vl);
					gPdolLen = vl;
				}
			} else if (lc <= sizeof(gPdol)) {
				/* Non-TLV PDOL data (unusual) - keep raw bytes */
				memcpy(gPdol, d, lc);
				gPdolLen = lc;
			}
		}
		note = 4;
		if (a) {
			tx = a->gpo;
			txLen = a->gpoLen;
			ok = reply(tx, txLen);
		} else {
			tx = SW_6A82;
			txLen = sizeof(SW_6A82);
			ok = reply(tx, txLen);
		}
	} else if (ins == 0xB2) {
		uint8_t sfi = p2 >> 3;
		const uint8_t* rd = nullptr;
		uint16_t rl = 0;
		bool found = false;
		if (p->sel == CE_SEL_PSE)
			found = findRecord(p->shared, p->numShared, sfi, p1, &rd, &rl);
		else {
			CeApp* a = ceProfileCurApp(p);
			if (a)
				found =
					findRecord(a->records, a->numRecords, sfi, p1, &rd, &rl);
		}
		note = 5;
		readSfi = sfi;
		readRec = p1;
		if (found) {
			tx = rd;
			txLen = rl;
			ok = reply(tx, txLen);
		} else {
			tx = SW_6A83;
			txLen = sizeof(SW_6A83);
			ok = reply(tx, txLen);
		}
	} else if (ins == 0xCA) {
		CeApp* a = ceProfileCurApp(p);
		if (!a) {
			tx = SW_6A88;
			txLen = sizeof(SW_6A88);
			ok = reply(tx, txLen);
		} else if (p1 == 0x9F && p2 == 0x36) {
			localBuf[0] = 0x9F;
			localBuf[1] = 0x36;
			localBuf[2] = 0x02;
			localBuf[3] = (uint8_t)(a->atc >> 8);
			localBuf[4] = (uint8_t)(a->atc & 0xFF);
			localBuf[5] = 0x90;
			localBuf[6] = 0x00;
			localLen = 7;
			tx = localBuf;
			txLen = localLen;
			ok = reply(tx, txLen);
		} else if (p1 == 0x9F && p2 == 0x17) {
			localBuf[0] = 0x9F;
			localBuf[1] = 0x17;
			localBuf[2] = 0x01;
			localBuf[3] = a->pinTry;
			localBuf[4] = 0x90;
			localBuf[5] = 0x00;
			localLen = 6;
			tx = localBuf;
			txLen = localLen;
			ok = reply(tx, txLen);
		} else {
			tx = SW_6A88;
			txLen = sizeof(SW_6A88);
			ok = reply(tx, txLen);
		}
	} else if (ins == 0xAE) {
		CeApp* a = ceProfileCurApp(p);
		note = 6;
		if (!a) {
			tx = SW_6A82;
			txLen = sizeof(SW_6A82);
			ok = reply(tx, txLen);
		} else if (!buildGenAc(a, cmd, cmdLen, localBuf, &localLen, &genLive,
							   &genCda, &genAtc)) {
			tx = SW_6985;
			txLen = sizeof(SW_6985);
			ok = reply(tx, txLen);
		} else {
			tx = localBuf;
			txLen = localLen;
			ok = reply(tx, txLen);
			if (ok) {
				a->atc = (uint16_t)(genAtc + 1);
				gAtcDirty = true;
			}
		}
	} else if (ins == 0x88) {
		CeApp* a = ceProfileCurApp(p);
		note = 7;
		if (!a) {
			tx = SW_6A88;
			txLen = sizeof(SW_6A88);
			ok = reply(tx, txLen);
		} else {
			ok = handleInternalAuth(a, cmd, cmdLen);
			txLen = 0;
		}
	} else if (ins == 0x82 || ins == 0x84) {
		tx = SW_6D00;
		txLen = sizeof(SW_6D00);
		ok = reply(tx, txLen);
	} else {
		tx = SW_6D00;
		txLen = sizeof(SW_6D00);
		ok = reply(tx, txLen);
	}

	uint8_t sw1 = 0, sw2 = 0;
	if (tx && txLen >= 2) {
		sw1 = tx[txLen - 2];
		sw2 = tx[txLen - 1];
	}
	evtPush(EVT_TX, ins, p1, (uint16_t)txLen, sw1, sw2, ok ? 1 : 0, note,
			readSfi, readRec);
	if (gOmitIad) {
		evtPush(EVT_NOTE, ins, 0, 0, 0, 0, 1, 8);
		gOmitIad = false;
	}

	noteApdu(ins, ok);
	if (ins == 0xAE && ok) {
		gHadGen = true;
		gGenLive = genLive;
		gHadCda = genCda;
		gGenAtc = genAtc;
	}
	return ok;
}

static void dumpSession(uint16_t apdus, uint8_t emptyStreak) {
	nfcLogAsTargetDeferred();
	for (uint8_t i = 0; i < gEvtN; i++) {
		const CeEvt* e = &gEvt[i];
		if (e->kind == EVT_RX)
			log("RX INS=%02X P1=%02X len=%u", e->ins, e->p1, e->len);
		else if (e->kind == EVT_TX) {
			const char* n = "";
			if (e->note == 1)
				n = " SEL=PPSE";
			else if (e->note == 2)
				n = " SEL=AID";
			else if (e->note == 3)
				n = " SEL=PSE";
			else if (e->note == 4)
				n = " GPO";
			else if (e->note == 5)
				n = " READ";
			else if (e->note == 6)
				n = " GENAC";
			else if (e->note == 7)
				n = " DDA";
			log("TX INS=%02X SW=%02X%02X len=%u ok=%u%s", e->ins, e->sw1,
				e->sw2, e->len, e->ok, n);
			if (e->note == 5) log("READ SFI=%u rec=%u", e->sfi, e->rec);
		} else if (e->kind == EVT_NOTE && e->note == 8)
			log("WARN GENAC omit_IAD");
	}
	for (uint8_t i = 0; i < gLogN; i++)
		log("APDU INS=%02X ok=%u", gInsLog[i], gOkLog[i]);
	if (gHadGen)
		log("GENAC live=%u CDA=%u ATC=%u", gGenLive ? 1 : 0, gHadCda ? 1 : 0,
			gGenAtc);
	if (gHadCda && gTdhInLen) {
		log("CDA TDH pdolLen=%u tdhInLen=%u "
			"tdh=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X"
			"%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
			gTdhPdolLen, gTdhInLen, gTdhDbg[0], gTdhDbg[1], gTdhDbg[2],
			gTdhDbg[3], gTdhDbg[4], gTdhDbg[5], gTdhDbg[6], gTdhDbg[7],
			gTdhDbg[8], gTdhDbg[9], gTdhDbg[10], gTdhDbg[11], gTdhDbg[12],
			gTdhDbg[13], gTdhDbg[14], gTdhDbg[15], gTdhDbg[16], gTdhDbg[17],
			gTdhDbg[18], gTdhDbg[19]);
	}
	if (gHadDda) log("DDA INTERNAL AUTH ok (ISO9796-2)");
	if (emptyStreak) log("ERR rf_empty streak=%u apdus=%u", emptyStreak, apdus);
	log("WRITE session end apdus=%u", apdus);
}

bool ceEmulateSession(CeProfile* p) {
	led(true);
	uint8_t emptyStreak = 0;
	uint16_t apdus = 0;
	gLogN = 0;
	gEvtN = 0;
	gHadGen = false;
	gHadCda = false;
	gHadDda = false;
	gAtcDirty = false;
	gOmitIad = false;
	gPdolLen = 0;
	p->sel = CE_SEL_NONE;

	for (uint16_t i = 0; i < 600; i++) {
		if (ceModeGet() != CE_MODE_WRITE) break;

		uint8_t cmd[CE_MAX_APDU_REC];
		uint8_t cmdLen = 0;
		memset(cmd, 0, sizeof(cmd));
		bool got = nfcGetData(cmd, &cmdLen);
		if (!got || cmdLen == 0) {
			emptyStreak++;
			uint8_t limit = apdus > 0 ? 40 : 24;
			if (emptyStreak >= limit) break;
			continue;
		}
		emptyStreak = 0;

		if (cmdLen >= 2 && cmd[0] == 0xFF && cmd[1] == 0xFF) continue;
		if (cmdLen >= 1 && cmd[0] == 0xE0) continue;
		if (cmdLen < 4 || (cmd[0] != 0x00 && cmd[0] != 0x80)) continue;
		if (!handleApdu(p, cmd, cmdLen)) break;
		apdus++;
	}
	led(false);

	if (gAtcDirty) ceProfilePersistAtc(p);

	dumpSession(apdus, emptyStreak);
	return true;
}
