/*
 * Copyright (c) 2026 Andrii Bilenko <bilenko.a.uni@gmail.com>.
 * All rights reserved.
 */

#pragma once
/**
 * @file ce_icc.h
 * @brief RSA offline dynamic authentication for the emulated chip (ICC)
 *
 * ce_crypto.h builds the 8-byte Application Cryptogram with DES
 * This file builds the big RSA signature the terminal recovers as tag 9F4B
 * (Signed Dynamic Application Data)
 *
 * Two cases:
 *   - DDA: terminal sends INTERNAL AUTHENTICATE with a challenge, we sign
 *     a block that contains an ICC Dynamic Number (IDN)
 *   - CDA: during GENERATE AC we sign a block that contains IDN, CID, AC,
 *     and a Transaction Data Hash (TDH)
 * BY ME TODO: isn't there supposed to be SDA 3rd case? also fix this big file
 * comment
 *
 * Signing format is ISO 9796-2 (header 0x6A, trailer 0xBC, SHA-1 in the
 * middle)
 */
#include <string.h>

#include "ce_profile.h"
#include "emv_des.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md.h"
#include "mbedtls/rsa.h"

/**
 * @brief Raw RSA private operation: out = in^D mod N.
 *
 * Uses the app ICC modulus N, private exponent D, and public E from the
 * profile. in and out are both nic bytes long (same size as N)
 *
 * @param a   App with ICC key material
 * @param in  Plain block to sign (must be less than N)
 * @param out Signature buffer (nic bytes)
 * @return false if keys missing or mbedtls fails, else true
 */
static bool rsa_private_raw(const CeApp* a, const uint8_t* in, uint8_t* out) {
	if (!a || !in || !out) return false;
	if (!a->iccNLen || a->iccNLen != a->iccDLen || !a->iccELen) return false;
	const size_t k = a->iccNLen;

	mbedtls_rsa_context rsa;
	mbedtls_mpi T, S, N, D;
	mbedtls_rsa_init(&rsa);
	mbedtls_mpi_init(&T);
	mbedtls_mpi_init(&S);
	mbedtls_mpi_init(&N);
	mbedtls_mpi_init(&D);

	bool ok =
		mbedtls_rsa_import_raw(&rsa, a->iccN, k, nullptr, 0, nullptr, 0,
							   a->iccD, k, a->iccE, a->iccELen) == 0 &&
		mbedtls_rsa_complete(&rsa) == 0 &&
		mbedtls_rsa_check_privkey(&rsa) == 0 &&
		mbedtls_rsa_export(&rsa, &N, nullptr, nullptr, &D, nullptr) == 0 &&
		mbedtls_mpi_read_binary(&T, in, k) == 0 &&
		mbedtls_mpi_cmp_mpi(&T, &N) < 0 &&
		mbedtls_mpi_exp_mod(&S, &T, &D, &N, nullptr) == 0 &&
		mbedtls_mpi_write_binary(&S, out, k) == 0;

	mbedtls_mpi_free(&T);
	mbedtls_mpi_free(&S);
	mbedtls_mpi_free(&N);
	mbedtls_mpi_free(&D);
	mbedtls_rsa_free(&rsa);
	return ok;
}

/**
 * @brief Build an ISO 9796-2 block and RSA-sign it (shared by DDA and CDA)
 *
 * Steps:
 *   1. hash = SHA-1(body + terminal_dynamic_data)
 *   2. block = 0x6A + body + hash + 0xBC  (length = key size nic)
 *   3. sigOut = RSA_private(block)
 *
 * body length must be nic - 22 (room for 1 + 20 + 1 header/hash/trailer)
 *
 * @param a          App with ICC private key
 * @param body       SDAD body (from 0x05 0x01 through pad)
 * @param bodyLen    Must equal nic - 22
 * @param termDyn    Extra bytes hashed with body (DDOL challenge when DDA, UN
 * when CDA)
 * @param termDynLen Length of termDyn
 * @param sigOut     Output signature
 * @param sigLen     In: buffer size, out: nic on success
 * @return false on size/key/hash/RSA failure, else true
 */
static bool emv_9796_sign(const CeApp* a, const uint8_t* body, size_t bodyLen,
						  const uint8_t* termDyn, size_t termDynLen,
						  uint8_t* sigOut, size_t* sigLen) {
	if (!a || !(a->flags & CE_FLAG_HAS_ICC)) return false;
	const size_t nic = a->iccNLen;
	if (nic < 64 || nic > CE_MAX_ICC || bodyLen != nic - 22) return false;

	uint8_t hashIn[512];
	size_t hn = 0;
	if (bodyLen + termDynLen > sizeof(hashIn)) return false;
	memcpy(hashIn, body, bodyLen);
	hn = bodyLen;
	if (termDyn && termDynLen) {
		memcpy(hashIn + hn, termDyn, termDynLen);
		hn += termDynLen;
	}

	uint8_t hash[20];
	const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
	if (!md || mbedtls_md(md, hashIn, hn, hash) != 0) return false;

	uint8_t block[CE_MAX_ICC];
	block[0] = 0x6A;
	memcpy(block + 1, body, bodyLen);
	memcpy(block + 1 + bodyLen, hash, 20);
	block[nic - 1] = 0xBC;

	if (*sigLen < nic) return false;
	if (!rsa_private_raw(a, block, sigOut)) return false;
	*sigLen = nic;
	return true;
}

/**
 * @brief Build the 8-byte ICC Dynamic Number (IDN) from ATC
 *
 * If the profile has MK-IDN: IDN = 3DES(MK-IDN, ATC then six zero bytes)
 * Else: IDN is just ATC then six zero bytes (no encryption)
 *
 * @param a    App (optional IDN master key)
 * @param atc  Application Transaction Counter
 * @param idn8 Output IDN
 */
static void mk_idn(const CeApp* a, uint16_t atc, uint8_t idn8[8]) {
	uint8_t blk[8];
	memset(blk, 0, 8);
	blk[0] = (uint8_t)(atc >> 8);
	blk[1] = (uint8_t)(atc & 0xFF);
	if (a && (a->flags & CE_FLAG_HAS_IDN))
		emv_tdes2_encrypt(a->mkIdn, blk, idn8);
	else
		memcpy(idn8, blk, 8);
}

/**
 * @brief DDA: sign for INTERNAL AUTHENTICATE (tag 9F4B)
 *
 * Builds a short dynamic-data body (header + IDN + 0xBB pad), then
 * 9796-2 signs it with the terminal DDOL challenge as termDyn.
 *
 * @param a       App with ICC key
 * @param atc     ATC for IDN
 * @param ddolData Challenge bytes from the terminal
 * @param ddolLen Length of challenge
 * @param sigOut  Output SDAD
 * @param sigLen  In/out signature length
 * @return false on bad args or sign failure
 */
static bool dda_sign(const CeApp* a, uint16_t atc, const uint8_t* ddolData,
					 size_t ddolLen, uint8_t* sigOut, size_t* sigLen) {
	if (!a || !ddolData || !ddolLen) return false;
	const size_t nic = a->iccNLen;
	const uint8_t ldn = 8;
	const uint8_t ldd = (uint8_t)(1 + ldn); /* 09 */
	if (nic < (size_t)(ldd + 25)) return false;
	const size_t pad = nic - ldd - 25;

	uint8_t idn[8];
	mk_idn(a, atc, idn);

	uint8_t body[CE_MAX_ICC];
	size_t o = 0;
	body[o++] = 0x05;
	body[o++] = 0x01;
	body[o++] = ldd;
	body[o++] = ldn;
	memcpy(body + o, idn, 8);
	o += 8;
	memset(body + o, 0xBB, pad);
	o += pad;
	if (o != nic - 22) return false;
	return emv_9796_sign(a, body, o, ddolData, ddolLen, sigOut, sigLen);
}

/**
 * @brief CDA: sign for GENERATE AC when the terminal asked for CDA
 *
 * 1. TDH = SHA-1(PDOL related || CDOL related || clear GEN AC TLVs)
 * 2. Body = header + IDN + CID + AC + TDH + 0xBB pad (FEIG LDD=38 layout)
 * 3. 9796-2 sign with Unpredictable Number (4 bytes) as termDyn
 *
 * Also copies TDH into gTdhDbg for deferred session logs
 *
 * @param a         App with ICC key
 * @param cid       Cryptogram Information Data (AC type only)
 * @param atc       ATC for IDN
 * @param ac8       Live 8-byte AC from ce_crypto
 * @param un4       Unpredictable Number from CDOL1
 * @param pdol      PDOL related data (value of GPO tag 83), may be empty
 * @param pdolLen   Length of pdol
 * @param cdol      CDOL1 related data from GEN AC
 * @param cdolLen   Length of cdol
 * @param rspTlv    Clear GEN AC response TLVs (no 9F4B), for TDH
 * @param rspTlvLen Length of rspTlv
 * @param sigOut    Output SDAD (9F4B value)
 * @param sigLen    In/out signature length
 * @return false on bad args, hash, or sign failure, else true
 */
static bool cda_sign(const CeApp* a, uint8_t cid, uint16_t atc,
					 const uint8_t ac8[8], const uint8_t un4[4],
					 const uint8_t* pdol, size_t pdolLen, const uint8_t* cdol,
					 size_t cdolLen, const uint8_t* rspTlv, size_t rspTlvLen,
					 uint8_t* sigOut, size_t* sigLen) {
	if (!a || !ac8 || !un4) return false;
	const size_t nic = a->iccNLen;

	uint8_t tdhIn[400];
	size_t tn = 0;
	if (pdol && pdolLen) {
		if (tn + pdolLen > sizeof(tdhIn)) return false;
		memcpy(tdhIn + tn, pdol, pdolLen);
		tn += pdolLen;
	}
	if (cdol && cdolLen) {
		if (tn + cdolLen > sizeof(tdhIn)) return false;
		memcpy(tdhIn + tn, cdol, cdolLen);
		tn += cdolLen;
	}
	if (rspTlv && rspTlvLen) {
		if (tn + rspTlvLen > sizeof(tdhIn)) return false;
		memcpy(tdhIn + tn, rspTlv, rspTlvLen);
		tn += rspTlvLen;
	}
	uint8_t tdh[20];
	const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
	if (!md || mbedtls_md(md, tdhIn, tn, tdh) != 0) return false;

	extern uint8_t gTdhDbg[20];
	extern uint16_t gTdhInLen;
	extern uint8_t gTdhPdolLen;
	memcpy(gTdhDbg, tdh, 20);
	gTdhInLen = (uint16_t)tn;
	gTdhPdolLen = (uint8_t)pdolLen;

	const uint8_t ldn = 8;
	/* LIDN(1)+IDN(8)+CID(1)+AC(8)+TDH(20) = 38 = 30+LIDN */
	const uint8_t ldd = (uint8_t)(1 + ldn + 1 + 8 + 20);
	if (nic < (size_t)(ldd + 25)) return false;
	const size_t pad = nic - ldd - 25;

	uint8_t idn[8];
	mk_idn(a, atc, idn);

	uint8_t body[CE_MAX_ICC];
	size_t o = 0;
	body[o++] = 0x05;
	body[o++] = 0x01;
	body[o++] = ldd;
	body[o++] = ldn;
	memcpy(body + o, idn, 8);
	o += 8;
	body[o++] = cid;
	memcpy(body + o, ac8, 8);
	o += 8;
	memcpy(body + o, tdh, 20);
	o += 20;
	memset(body + o, 0xBB, pad);
	o += pad;
	if (o != nic - 22) return false;
	return emv_9796_sign(a, body, o, un4, 4, sigOut, sigLen);
}

/**
 * @brief Pull the 4-byte Unpredictable Number (tag 9F37) out of CDOL1 data.
 *
 * Walks the CDOL1 template to find where 9F37 sits in the GEN AC payload
 * If the template has no 9F37, falls back to offset 25 when lc >= 29
 *
 * @param a        App with cdol1 template
 * @param cdolData Raw CDOL1-related bytes from the GEN AC APDU
 * @param lc       Length of cdolData
 * @param un4      Output UN (4 bytes)
 * @return false if UN cannot be located, else true
 */
static bool cdol1_un(const CeApp* a, const uint8_t* cdolData, uint8_t lc,
					 uint8_t un4[4]) {
	memset(un4, 0, 4);
	if (!a || !cdolData) return false;
	size_t off = 0;
	size_t i = 0;
	while (i < a->cdol1Len) {
		uint16_t tag = a->cdol1[i++];
		if ((tag & 0x1F) == 0x1F) {
			if (i >= a->cdol1Len) break;
			tag = (uint16_t)((tag << 8) | a->cdol1[i++]);
		}
		if (i >= a->cdol1Len) break;
		uint8_t L = a->cdol1[i++];
		if (tag == 0x9F37) {
			if (off + 4 <= lc) {
				memcpy(un4, cdolData + off, 4);
				return true;
			}
			return false;
		}
		off += L;
	}
	if (lc >= 29) {
		memcpy(un4, cdolData + 25, 4);
		return true;
	}
	return false;
}
