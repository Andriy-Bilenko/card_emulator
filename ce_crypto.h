#pragma once
/**
 * @file ce_crypto.h
 * @brief Card-side Application Cryptogram (AC) helpers for PICC emulate
 *
 * Builds a live 8-byte AC from the profile MasterKeyAC (imkAc):
 *   PAN+PSN -> Y -> UDK -> ATC session key -> ISO 9797-style Retail MAC over
 *   GEN AC / CDOL1 transaction data
 * Uses DES / 2-key 3DES
 */
#include <string.h>

#include "ce_profile.h"
#include "emv_des.h"

/**
 * @brief Unpack Application PAN into an ASCII digit string
 *
 * @param a   App with pan[] / panLen
 * @param out Buffer for digits + NUL (max 19 digits)
 */
static void pan_digits(const CeApp* a, char out[20]) {
	size_t n = 0;
	for (uint8_t i = 0; i < a->panLen && n + 1 < 20; i++) {
		uint8_t hi = (a->pan[i] >> 4) & 0x0F;
		uint8_t lo = a->pan[i] & 0x0F;
		if (hi <= 9) out[n++] = (char)('0' + hi);
		if (lo <= 9 && n + 1 < 20) out[n++] = (char)('0' + lo);
	}
	out[n] = 0;
}

/**
 * @brief Build the 8-byte PAN+PSN block Y used for Option-A UDK derivation
 *
 * @param a    App with PAN and psn
 * @param out8 Result Y block
 */
static void pan_psn_block(const CeApp* a, uint8_t out8[8]) {
	char digs_in[20];
	pan_digits(a, digs_in);
	size_t n = strlen(digs_in);
	char digs[16];
	memset(digs, '0', sizeof(digs));
	if (n >= 2) {
		size_t body = n - 1;
		size_t take = body > 14 ? 14 : body;
		memcpy(digs + (14 - take), digs_in + (body - take), take);
	}
	digs[14] = '0';
	digs[15] = (char)('0' + (a->psn % 10));
	for (int i = 0; i < 8; i++) {
		int hi = digs[i * 2] - '0';
		int lo = digs[i * 2 + 1] - '0';
		out8[i] = (uint8_t)((hi << 4) | lo);
	}
}

/**
 * @brief Derive 16-byte Unique DEA Key (UDK) from MasterKeyAC and PAN/PSN
 *
 * UDK_L = 3DES(IMK, Y), UDK_R = 3DES(IMK, Y xor FF…FF)
 *
 * @param a     App with imkAc and PAN/PSN
 * @param udk16 Output UDK
 * @return false if a/udk16 null, no IMK, or empty PAN, else true
 */
static bool derive_udk(const CeApp* a, uint8_t udk16[16]) {
	if (!a || !udk16) return false;
	if (!(a->flags & CE_FLAG_HAS_IMK) || a->panLen == 0) return false;
	uint8_t y[8];
	pan_psn_block(a, y);
	emv_tdes2_encrypt(a->imkAc, y, udk16);
	uint8_t yx[8];
	for (int i = 0; i < 8; i++) yx[i] = (uint8_t)(y[i] ^ 0xFF);
	emv_tdes2_encrypt(a->imkAc, yx, udk16 + 8);
	return true;
}

/**
 * @brief Derive 16-byte AC session key from UDK and ATC
 *
 * SK left  = 3DES(UDK, ATC then 0xF0 then zeros)
 * SK right = 3DES(UDK, ATC then 0x0F then zeros)
 *
 * @param udk16 Card UDK
 * @param atc   Application Transaction Counter
 * @param sk16  Output session key
 * @return false if udk16 or sk16 is null, else true
 */
static bool session_key(const uint8_t udk16[16], uint16_t atc,
						uint8_t sk16[16]) {
	if (!udk16 || !sk16) return false;
	uint8_t r[8];
	memset(r, 0, 8);
	r[0] = (uint8_t)(atc >> 8);
	r[1] = (uint8_t)(atc & 0xFF);
	r[2] = 0xF0;
	emv_tdes2_encrypt(udk16, r, sk16);
	r[2] = 0x0F;
	emv_tdes2_encrypt(udk16, r, sk16 + 8);
	return true;
}

/**
 * @brief Last AC step: MAC the padded CDOL bytes with the session key
 *
 * Appends 0x80 and zero-pad to a multiple of 8, then Retail MAC with DES
 * Output mac8 is the 8-byte Application Cryptogram
 *
 * @param sk16 Session key
 * @param data Transaction / CDOL-related bytes to MAC
 * @param len  Length of data
 * @param mac8 Output 8-byte cryptogram
 * @return false if data too long for the internal buffer, else true
 */
static bool mac_ac(const uint8_t sk16[16], const uint8_t* data, size_t len,
				   uint8_t mac8[8]) {
	uint8_t buf[256];
	if (len + 8 > sizeof(buf)) return false;
	memcpy(buf, data, len);
	size_t n = len;
	buf[n++] = 0x80;
	while (n % 8) buf[n++] = 0x00;
	uint8_t y[8];
	memset(y, 0, 8);
	for (size_t off = 0; off < n; off += 8) {
		uint8_t x[8];
		for (int i = 0; i < 8; i++) x[i] = (uint8_t)(y[i] ^ buf[off + i]);
		emv_des_encrypt(sk16, x, y);
	}
	uint8_t t1[8], t2[8];
	emv_des_decrypt(sk16 + 8, y, t1);
	emv_des_encrypt(sk16, t1, t2);
	memcpy(mac8, t2, 8);
	return true;
}

/**
 * @brief Full AC creation: UDK, then session key, then MAC (calls the three
 * helpers)
 *
 * @param a     App (needs imkAc, PAN, PSN)
 * @param atc   ATC for this transaction
 * @param tx    Bytes to MAC (CDOL1-related data)
 * @param txLen Length of tx
 * @param ac8   Output AC
 * @return false if any step fails
 */
static bool compute_ac(const CeApp* a, uint16_t atc, const uint8_t* tx,
					   size_t txLen, uint8_t ac8[8]) {
	uint8_t udk[16], sk[16];
	if (!derive_udk(a, udk)) return false;
	if (!session_key(udk, atc, sk)) return false;
	return mac_ac(sk, tx, txLen, ac8);
}

/**
 * @brief Sum of data lengths listed in the app CDOL1 template
 *
 * Walks tag(+optional second tag byte) then length byte, returns how many
 * CDOL1 data bytes GEN AC should supply for MAC / parsing
 *
 * @param a App with cdol1[] / cdol1Len
 * @return how many CDOL1 data bytes GEN AC should supply for MAC / parsing
 */
static size_t cdol1_data_len(const CeApp* a) {
	size_t total = 0;
	size_t i = 0;
	while (i < a->cdol1Len) {
		uint8_t b0 = a->cdol1[i++];
		if ((b0 & 0x1F) == 0x1F) {
			if (i >= a->cdol1Len) break;
			i++;
		}
		if (i >= a->cdol1Len) break;
		total += a->cdol1[i++];
	}
	return total;
}
