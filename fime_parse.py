#!/usr/bin/env python3
#
# Copyright (c) 2026 Andrii Bilenko <bilenko.a.uni@gmail.com>.
# All rights reserved.
#

"""Collis CardImage (Type A / Kind A) → T6P5 binary profile for card emulator PICC player.

Strict: no invented AIP/AFL/IAD/AC. Missing required data → ValueError.
"""

# BY ME TODO: rename to card_image_parse.py, read and figure out if this file is even used, understand/clean up

from __future__ import annotations

import struct
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

MAGIC = b"T6P5"
VERSION = 5
FLAG_HAS_IMK = 0x01
FLAG_HAS_LIVE_AC = 0x02
FLAG_HAS_PSE = 0x04
FLAG_HAS_ICC = 0x08
FLAG_HAS_IAD = 0x10
FLAG_HAS_IDN = 0x40
FLAG_PPSE_DERIVED = 0x20  # profile-level: PPSE synthesized from apps

# Wire / firmware caps — must match ce_profile.h (CE_MAX_*)
MAX_BLOB = 8192
MAX_APPS = 5
MAX_RECORDS_TOTAL = 32
MAX_SHARED_REC = 8
MAX_APP_REC = 16
MAX_AID = 16
MAX_FCI = 200
MAX_GPO = 100
MAX_REC = 253  # CE_MAX_APDU_REC: Adafruit uint8_t LEN - TFI+0x8E+APDU ≤255
MAX_PAN = 10
MAX_CDOL = 64
MAX_ICC = 256
MAX_IAD = 32

NAME_TO_TAG: Dict[str, int] = {
    "AIP": 0x82,
    "ApplicationCurrencyCode": 0x9F42,
    "ApplicationCurrencyExponent": 0x9F44,
    "ApplicationEffectiveDate": 0x5F25,
    "ApplicationExpirationDate": 0x5F24,
    "ApplicationLabel": 0x50,
    "ApplicationPAN": 0x5A,
    "ApplicationPANSequenceNumber": 0x5F34,
    "ApplicationPANSeqno": 0x5F34,  # MCHIP alias
    "ApplicationPreferredName": 0x9F12,
    "ApplicationPriorityIndicator": 0x87,
    "ApplicationReferenceCurrency": 0x9F3B,
    "ApplicationReferenceCurrencyExponent": 0x9F43,
    "ApplicationUsageControl": 0x9F07,
    "AUC": 0x9F07,  # MCHIP alias
    "ApplicationVersionNumber": 0x9F08,
    "CardholderName": 0x5F20,
    "CardHolderName": 0x5F20,  # MCHIP alias
    "CDOL1": 0x8C,
    "CDOL2": 0x8D,
    "CVMList": 0x8E,
    "DDOL": 0x9F49,
    "IssuerActionCodeDefault": 0x9F0D,
    "IACDefault": 0x9F0D,
    "IssuerActionCodeDenial": 0x9F0E,
    "IACDenial": 0x9F0E,
    "IssuerActionCodeOnline": 0x9F0F,
    "IACOnline": 0x9F0F,
    "IssuerApplicationData": 0x9F10,
    "IssuerCodeTableIndex": 0x9F11,
    "IssuerCountryCode": 0x5F28,
    "IssuerURL": 0x5F50,
    "LanguagePreference": 0x5F2D,
    "LogEntry": 0x9F4D,
    "SDATagList": 0x9F4A,
    "ServiceCode": 0x5F30,
    "SignedStaticApplicationData": 0x93,
    "Track2DiscretionaryData": 0x9F20,
    "Track2EquivalentData": 0x57,
    "CAPublicKeyIndex": 0x8F,
    "IssuerPublicKeyCertificate": 0x90,
    "IssuerPublicKeyRemainder": 0x92,
    "IssuerPublicKeyExponent": 0x9F32,
    "IccPublicKeyCertificate": 0x9F46,
    "IccPublicKeyExponent": 0x9F47,
    "IccPublicKeyRemainder": 0x9F48,
    "MagstripeApplicationVersionNumber": 0x9F6C,
    "MagStripeApplicationVersionNumber": 0x9F6C,
    "ApplicationVersionNumberMagstripe": 0x9F6C,
}


def local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1] if "}" in tag else tag


def parse_hex(text: Optional[str]) -> bytes:
    if not text:
        return b""
    parts = text.replace("\n", " ").replace("\t", " ").split()
    out = bytearray()
    for p in parts:
        p = p.strip()
        if not p:
            continue
        if len(p) % 2:
            p = "0" + p
        out.extend(bytes.fromhex(p))
    return bytes(out)


def tag_bytes(tag: int) -> bytes:
    if tag <= 0xFF:
        return bytes([tag])
    return bytes([(tag >> 8) & 0xFF, tag & 0xFF])


def tlv(tag: int, value: bytes) -> bytes:
    tb = tag_bytes(tag)
    n = len(value)
    if n < 0x80:
        return tb + bytes([n]) + value
    if n <= 0xFF:
        return tb + bytes([0x81, n]) + value
    return tb + bytes([0x82, (n >> 8) & 0xFF, n & 0xFF]) + value


def parse_tag_text(s: str) -> int:
    b = parse_hex(s)
    if not b:
        raise ValueError(f"empty tag text {s!r}")
    v = 0
    for x in b:
        v = (v << 8) | x
    return v


def find_child(parent: ET.Element, name: str) -> Optional[ET.Element]:
    for c in parent:
        if local(c.tag) == name:
            return c
    return None


def find_text(parent: ET.Element, name: str) -> Optional[str]:
    el = find_child(parent, name)
    return el.text if el is not None else None


def children_named(parent: ET.Element, name: str) -> List[ET.Element]:
    return [c for c in parent if local(c.tag) == name]


@dataclass
class Record:
    sfi: int
    rec: int
    data: bytes


@dataclass
class AppProfile:
    aid: bytes = b""
    aid_fci: bytes = b""
    gpo: bytes = b""
    records: List[Record] = field(default_factory=list)
    pan: bytes = b""
    psn: int = 0
    imk_ac: bytes = b""
    mk_idn: bytes = b""
    cdol1: bytes = b""
    cdol2: bytes = b""
    iad: bytes = b""
    atc: int = 1
    pin_try: int = 3
    aip: bytes = b""
    afl: bytes = b""
    flags: int = 0
    label: bytes = b""
    prio: bytes = b"\x01"
    icc_n: bytes = b""
    icc_d: bytes = b""
    icc_e: bytes = b""


@dataclass
class Profile:
    ppse: bytes = b""
    pse: bytes = b""
    shared_records: List[Record] = field(default_factory=list)
    apps: List[AppProfile] = field(default_factory=list)
    flags: int = 0
    ppse_source: str = "derived"  # "xml" | "derived"


def collect_named_hex_deep(parent: ET.Element) -> Dict[str, bytes]:
    """Leaf hex from element text and/or Text= attribute (Collis SSAD etc.)."""
    out: Dict[str, bytes] = {}
    for el in parent.iter():
        name = local(el.tag)
        if name in ("Tag", "Template", "InjectedData"):
            continue
        attr_raw = el.attrib.get("Text")
        attr_hex = parse_hex(attr_raw) if attr_raw is not None else b""
        text_hex = parse_hex(el.text) if el.text else b""
        if attr_raw is not None:
            # Prefer Text= even when empty (explicit); non-empty wins over child text
            out[name] = attr_hex
        elif len(el) == 0:
            out[name] = text_hex
    return out


def build_from_template(node: ET.Element, values: Dict[int, bytes]) -> bytes:
    """Build TLV body; honor Collis InjectedData (raw bytes After/Before tag)."""
    # Before-injections keyed by tag int
    before: Dict[int, bytes] = {}
    for child in list(node):
        if local(child.tag) != "InjectedData":
            continue
        if (child.attrib.get("Place") or "After").strip().lower() != "before":
            continue
        t = child.attrib.get("Tag")
        if not t:
            continue
        before[parse_tag_text(t)] = parse_hex(child.attrib.get("Text") or child.text)

    out = bytearray()
    for child in list(node):
        name = local(child.tag)
        if name == "Tag":
            tag = parse_tag_text(child.text or "")
            if tag in before:
                out.extend(before[tag])
            val = values.get(tag, b"")
            out.extend(tlv(tag, val))
        elif name == "Template":
            ttag = parse_tag_text(child.attrib.get("TemplateTag", "70"))
            inner = build_from_template(child, values)
            out.extend(tlv(ttag, inner))
        elif name == "InjectedData":
            place = (child.attrib.get("Place") or "After").strip().lower()
            raw = parse_hex(child.attrib.get("Text") or child.text)
            if raw and place == "after":
                out.extend(raw)
    return bytes(out)


def values_from_names(named: Dict[str, bytes]) -> Dict[int, bytes]:
    vals: Dict[int, bytes] = {}
    for name, raw in named.items():
        tag = NAME_TO_TAG.get(name)
        if tag is not None and raw:
            vals[tag] = raw
    return vals


def merge_keys(keys_el: Optional[ET.Element], vals: Dict[int, bytes]) -> bytes:
    if keys_el is None:
        return b""
    named = collect_named_hex_deep(keys_el)
    mapping = {
        "CAPublicKeyIndex": 0x8F,
        "IssuerPublicKeyCertificate": 0x90,
        "IssuerPublicKeyRemainder": 0x92,
        "IssuerPublicKeyExponent": 0x9F32,
        "IccPublicKeyCertificate": 0x9F46,
        "IccPublicKeyExponent": 0x9F47,
        "IccPublicKeyRemainder": 0x9F48,
        "SignedStaticApplicationData": 0x93,
    }
    for name, tag in mapping.items():
        if name in named and named[name] and tag not in vals:
            vals[tag] = named[name]
    return named.get("MasterKeyAC", b"")


def _usable_rsa_blob(b: bytes) -> bool:
    if len(b) < 64 or len(b) > MAX_ICC:
        return False
    zeros = sum(1 for x in b if x == 0)
    return zeros < len(b) // 2


def extract_icc_rsa(keys_el: Optional[ET.Element]) -> Tuple[bytes, bytes, bytes]:
    if keys_el is None:
        return b"", b"", b""
    named = collect_named_hex_deep(keys_el)
    n = named.get("IccPublicModulus") or named.get("IccPublicKeyModulus") or b""
    d = named.get("IccPrivateKey") or b""
    e = named.get("IccPublicKeyExponent") or b"\x03"
    if not _usable_rsa_blob(n) or not _usable_rsa_blob(d):
        return b"", b"", b""
    if len(n) != len(d):
        return b"", b"", b""
    if not e:
        e = b"\x03"
    if len(e) > 3:
        e = e[-3:]
    return n, d, e


def parse_afl_from_app(app: ET.Element) -> bytes:
    afl = parse_hex(find_text(app, "AFL"))
    if afl:
        return afl
    aflist = find_child(app, "AFLList")
    if aflist is not None:
        preferred = None
        first = None
        for e in children_named(aflist, "AFLEntry"):
            raw = parse_hex(e.attrib.get("Text") or e.text)
            if not raw:
                continue
            if first is None:
                first = raw
            typ = (e.attrib.get("Type") or "").upper()
            if any(
                x in typ
                for x in ("QVSDC", "VSDC", "CONTACTLESS", "CL", "PAYPASS", "PAYWAVE")
            ):
                preferred = raw
                break
        return preferred or first or b""
    return b""


def afl_from_records(records: List[Record]) -> bytes:
    by_sfi: Dict[int, List[int]] = {}
    for r in records:
        by_sfi.setdefault(r.sfi, []).append(r.rec)
    out = bytearray()
    for sfi in sorted(by_sfi):
        recs = sorted(by_sfi[sfi])
        out.append((sfi & 0x1F) << 3)
        out.append(recs[0] & 0xFF)
        out.append(recs[-1] & 0xFF)
        out.append(0)
    return bytes(out)


def build_ppse(apps: List[AppProfile]) -> bytes:
    entries = bytearray()
    for a in apps:
        label = a.label or b"APP"
        prio = a.prio or b"\x01"
        entry = tlv(0x4F, a.aid) + tlv(0x50, label) + tlv(0x87, prio)
        entries.extend(tlv(0x61, entry))
    bf0c = tlv(0xBF0C, bytes(entries))
    a5 = tlv(0xA5, bf0c)
    fci = tlv(0x6F, tlv(0x84, b"2PAY.SYS.DDF01") + a5)
    return fci + b"\x90\x00"


def build_pse_fci(dir_id: bytes, lang: bytes, sfi: bytes) -> bytes:
    a5 = bytearray()
    if lang:
        a5.extend(tlv(0x5F2D, lang))
    if sfi:
        a5.extend(tlv(0x88, sfi))
    a5.extend(tlv(0x9F11, b"\x01"))
    fci = tlv(0x6F, tlv(0x84, dir_id) + tlv(0xA5, bytes(a5)))
    return fci + b"\x90\x00"


def build_gpo(aip: bytes, afl: bytes) -> bytes:
    body = tlv(0x82, aip) + tlv(0x94, afl)
    return tlv(0x77, body) + b"\x90\x00"


def build_aid_fci(
    app: ET.Element,
    aid: bytes,
    vals: Dict[int, bytes],
    label: bytes,
    prio: bytes,
    lang: bytes,
) -> bytes:
    rt = find_child(app, "RecordTemplates")
    fci_tpl = find_child(rt, "FCITemplate") if rt is not None else None
    if fci_tpl is not None and any(local(c.tag) == "Template" for c in fci_tpl):
        vals = dict(vals)
        vals[0x84] = aid
        vals.setdefault(0x50, label)
        vals.setdefault(0x87, prio)
        vals.setdefault(0x5F2D, lang)
        vals.setdefault(0x9F12, label)
        vals.setdefault(0x9F11, b"\x01")
        body = build_from_template(fci_tpl, vals)
        if body.startswith(b"\x6F"):
            return body + b"\x90\x00"
        return tlv(0x6F, body) + b"\x90\x00"
    a5 = (
        tlv(0x50, label)
        + tlv(0x5F2D, lang)
        + tlv(0x87, prio)
        + tlv(0x9F11, b"\x01")
        + tlv(0x9F12, label)
    )
    return tlv(0x6F, tlv(0x84, aid) + tlv(0xA5, a5)) + b"\x90\x00"


def parse_records(rt: Optional[ET.Element], vals: Dict[int, bytes]) -> List[Record]:
    out: List[Record] = []
    if rt is None:
        return out
    for rec_t in children_named(rt, "RecordTemplate"):
        sfi = int(rec_t.attrib.get("ShortFileIdentifier", "0"), 16)
        recn = int(rec_t.attrib.get("RecordNumber", "0"), 16)
        body = build_from_template(rec_t, vals)
        if not body.startswith(b"\x70"):
            body = tlv(0x70, body)
        out.append(Record(sfi, recn, body + b"\x90\x00"))
    return out


def parse_ppse_xml(ppse_el: ET.Element, apps: List[AppProfile]) -> bytes:
    """Build PPSE FCI from XML PPSE templates when present."""
    rt = find_child(ppse_el, "RecordTemplates")
    vals: Dict[int, bytes] = {0x84: b"2PAY.SYS.DDF01"}
    if apps:
        vals[0x4F] = apps[0].aid
        vals[0x50] = apps[0].label or b"APP"
        vals[0x87] = apps[0].prio or b"\x01"
        vals[0x9F12] = apps[0].label or b"APP"
    # Multi-app: still use first-app values in template tags; directory built from apps if needed
    if rt is not None:
        ft = find_child(rt, "FCITemplate")
        if ft is not None and any(local(c.tag) == "Template" for c in ft):
            body = build_from_template(ft, vals)
            fci = body if body.startswith(b"\x6F") else tlv(0x6F, body)
            return fci + b"\x90\x00"
    raise ValueError("PPSE present but no usable FCITemplate")


def parse_one_app(app: ET.Element) -> AppProfile:
    aid = parse_hex(find_text(app, "AID"))
    if not aid:
        raise ValueError("Application missing AID")
    if len(aid) > MAX_AID:
        raise ValueError(f"AID len {len(aid)} > {MAX_AID}")

    named: Dict[str, bytes] = {}
    for c in app:
        name = local(c.tag)
        if name.endswith("ApplicationData") or name in (
            "PersistentData",
            "TransientData",
        ):
            named.update(collect_named_hex_deep(c))
        if name in ("PersistentData", "TransientData"):
            for sub in c:
                named.update(collect_named_hex_deep(sub))

    for c in app.iter():
        if local(c.tag).endswith("ApplicationData"):
            named.update(collect_named_hex_deep(c))

    vals = values_from_names(named)
    keys_el = find_child(app, "Keys")
    imk = merge_keys(keys_el, vals)
    icc_n, icc_d, icc_e = extract_icc_rsa(keys_el)
    mk_idn = b""
    if keys_el is not None:
        kn = collect_named_hex_deep(keys_el)
        cand = kn.get("MasterKeyIDN") or b""
        if len(cand) == 16:
            mk_idn = cand

    aip = vals.get(0x82) or named.get("AIP", b"")
    if not aip:
        raise ValueError(f"AID={aid.hex().upper()}: missing AIP (will not invent)")

    label = vals.get(0x50) or named.get("ApplicationLabel") or b"APP"
    prio = vals.get(0x87) or named.get("ApplicationPriorityIndicator") or b"\x01"
    lang = vals.get(0x5F2D) or named.get("LanguagePreference") or b"en"

    rt = find_child(app, "RecordTemplates")
    records = parse_records(rt, vals)
    if len(records) > MAX_APP_REC:
        raise ValueError(f"app records {len(records)} > {MAX_APP_REC}")

    afl = parse_afl_from_app(app)
    if not afl:
        afl = afl_from_records(records)
    if not afl:
        raise ValueError(
            f"AID={aid.hex().upper()}: missing AFL and no records to derive AFL"
        )

    aid_fci = build_aid_fci(app, aid, vals, label, prio, lang)
    gpo = build_gpo(aip, afl)

    for name, blob, lim in (
        ("FCI", aid_fci, MAX_FCI),
        ("GPO", gpo, MAX_GPO),
    ):
        if len(blob) > lim:
            raise ValueError(f"{name} len {len(blob)} > {lim}")
    for r in records:
        if len(r.data) > MAX_REC:
            raise ValueError(
                f"record SFI{r.sfi}/{r.rec} len {len(r.data)} > {MAX_REC}"
            )

    pan = vals.get(0x5A, b"")
    if len(pan) > MAX_PAN:
        raise ValueError(f"PAN len {len(pan)} > {MAX_PAN} (will not truncate)")
    psn_b = vals.get(0x5F34, b"\x00")
    psn = psn_b[0] if psn_b else 0
    cdol1 = vals.get(0x8C, b"")
    if len(cdol1) > MAX_CDOL:
        raise ValueError(f"CDOL1 len {len(cdol1)} > {MAX_CDOL}")
    cdol2 = vals.get(0x8D, b"")
    if len(cdol2) > MAX_CDOL:
        raise ValueError(f"CDOL2 len {len(cdol2)} > {MAX_CDOL}")

    iad = vals.get(0x9F10) or named.get("IssuerApplicationData", b"")
    if len(iad) > MAX_IAD:
        raise ValueError(f"IAD len {len(iad)} > {MAX_IAD}")

    atc_b = named.get("ATC", b"\x00\x01")
    atc = int.from_bytes(atc_b[:2].ljust(2, b"\x00"), "big") or 1
    pin_try_b = named.get("PINTryCounter", b"\x03")
    pin_try = pin_try_b[0] if pin_try_b else 3

    flags = 0
    if len(imk) == 16:
        flags |= FLAG_HAS_IMK | FLAG_HAS_LIVE_AC
    if icc_n and icc_d:
        flags |= FLAG_HAS_ICC
    if iad:
        flags |= FLAG_HAS_IAD
    if len(mk_idn) == 16:
        flags |= FLAG_HAS_IDN

    # AIP byte1 (EMV Book 3): b7=0x40 DDA, b1=0x01 CDA - need ICC key material
    aip0 = aip[0] if aip else 0
    if (aip0 & 0x41) and not (flags & FLAG_HAS_ICC):
        raise ValueError(
            f"AID={aid.hex().upper()}: AIP={aip.hex().upper()} needs DDA/CDA "
            f"but usable IccPublicModulus/IccPrivateKey missing"
        )

    return AppProfile(
        aid=aid,
        aid_fci=aid_fci,
        gpo=gpo,
        records=records,
        pan=pan,
        psn=psn,
        imk_ac=imk if len(imk) == 16 else b"",
        mk_idn=mk_idn,
        cdol1=cdol1,
        cdol2=cdol2,
        iad=iad,
        atc=atc,
        pin_try=pin_try,
        aip=aip,
        afl=afl,
        flags=flags,
        label=label,
        prio=prio,
        icc_n=icc_n,
        icc_d=icc_d,
        icc_e=icc_e,
    )


def _u8_blob(buf: bytearray, blob: bytes, what: str) -> None:
    if len(blob) > 255:
        raise ValueError(f"{what} too large for u8 length")
    buf.append(len(blob))
    buf.extend(blob)


def _u16_rec(buf: bytearray, r: Record) -> None:
    if len(r.data) > MAX_REC:
        raise ValueError("record too long")
    buf.append(r.sfi & 0xFF)
    buf.append(r.rec & 0xFF)
    buf.extend(struct.pack(">H", len(r.data)))
    buf.extend(r.data)


def pack_profile(p: Profile) -> bytes:
    buf = bytearray()
    buf.extend(MAGIC)
    buf.append(VERSION)
    buf.append(p.flags & 0xFF)
    _u8_blob(buf, p.ppse, "PPSE")
    _u8_blob(buf, p.pse, "PSE")

    if len(p.shared_records) > MAX_SHARED_REC:
        raise ValueError(f"shared records {len(p.shared_records)} > {MAX_SHARED_REC}")
    buf.append(len(p.shared_records))
    for r in p.shared_records:
        _u16_rec(buf, r)

    if not p.apps:
        raise ValueError("no applications")
    if len(p.apps) > MAX_APPS:
        raise ValueError(f"apps {len(p.apps)} > {MAX_APPS}")
    buf.append(len(p.apps))

    total_rec = len(p.shared_records)
    for a in p.apps:
        total_rec += len(a.records)
        buf.append(a.flags & 0xFF)
        _u8_blob(buf, a.aid, "AID")
        _u8_blob(buf, a.aid_fci, "AID FCI")
        _u8_blob(buf, a.gpo, "GPO")
        if len(a.records) > MAX_APP_REC:
            raise ValueError(f"app records > {MAX_APP_REC}")
        buf.append(len(a.records))
        for r in a.records:
            _u16_rec(buf, r)
        _u8_blob(buf, a.pan, "PAN")
        buf.append(a.psn & 0xFF)
        imk = a.imk_ac if len(a.imk_ac) == 16 else bytes(16)
        buf.extend(imk)
        mk_idn = a.mk_idn if len(a.mk_idn) == 16 else bytes(16)
        buf.extend(mk_idn)
        _u8_blob(buf, a.cdol1, "CDOL1")
        _u8_blob(buf, a.cdol2, "CDOL2")
        _u8_blob(buf, a.iad, "IAD")
        buf.extend(struct.pack(">H", a.atc & 0xFFFF))
        buf.append(a.pin_try & 0xFF)
        if a.flags & FLAG_HAS_ICC:
            _u8_blob(buf, a.icc_n, "ICC N")
            _u8_blob(buf, a.icc_d, "ICC D")
            _u8_blob(buf, a.icc_e, "ICC E")
        else:
            buf.append(0)
            buf.append(0)
            buf.append(0)

    if total_rec > MAX_RECORDS_TOTAL:
        raise ValueError(f"total records {total_rec} > {MAX_RECORDS_TOTAL}")
    if len(buf) > MAX_BLOB:
        raise ValueError(f"blob {len(buf)} > {MAX_BLOB}")
    return bytes(buf)


def parse_cardimage(path: Path) -> Profile:
    root = ET.parse(path).getroot()
    rtag = local(root.tag)
    if rtag == "EMVCoL3CardImage":
        raise ValueError("EMVCoL3CardImage not supported (Type B / L3 script)")
    if rtag == "ProfilePerso":
        raise ValueError("ProfilePerso not supported (pointer only)")
    if rtag != "CardImage":
        raise ValueError(f"root is {rtag}, expected CardImage")

    app_els = children_named(root, "Application")
    if not app_els:
        raise ValueError("no Application (magstripe-only or empty CardImage)")
    if len(app_els) > MAX_APPS:
        raise ValueError(f"apps {len(app_els)} > {MAX_APPS} (will not drop extras)")

    apps: List[AppProfile] = [parse_one_app(el) for el in app_els]

    def prio_key(a: AppProfile) -> int:
        return a.prio[0] if a.prio else 0xFF

    apps.sort(key=prio_key)

    shared: List[Record] = []
    pse_fci = b""
    flags = 0
    pse_el = find_child(root, "PSE")
    if pse_el is not None:
        dir_id = parse_hex(find_text(pse_el, "DirectoryIdentifier")) or b"1PAY.SYS.DDF01"
        lang = b"en"
        pse_rt = find_child(pse_el, "RecordTemplates")
        pse_vals: Dict[int, bytes] = {
            0x84: dir_id,
            0x5F2D: lang,
            0x88: b"\x01",
            0x9F11: b"\x01",
        }
        if apps:
            pse_vals[0x4F] = apps[0].aid
            pse_vals[0x50] = apps[0].label or b"APP"
            pse_vals[0x87] = apps[0].prio or b"\x01"
            pse_vals[0x9F12] = apps[0].label or b"APP"
        if pse_rt is not None:
            ft = find_child(pse_rt, "FCITemplate")
            if ft is not None and any(local(c.tag) == "Template" for c in ft):
                body = build_from_template(ft, pse_vals)
                pse_fci = (
                    body if body.startswith(b"\x6F") else tlv(0x6F, body)
                ) + b"\x90\x00"
            shared = parse_records(pse_rt, pse_vals)
        if not pse_fci:
            pse_fci = build_pse_fci(dir_id, lang, b"\x01")
        flags |= FLAG_HAS_PSE

    if len(shared) > MAX_SHARED_REC:
        raise ValueError(f"PSE records {len(shared)} > {MAX_SHARED_REC}")

    ppse_source = "derived"
    ppse_el = find_child(root, "PPSE")
    if ppse_el is not None:
        try:
            ppse = parse_ppse_xml(ppse_el, apps)
            ppse_source = "xml"
        except ValueError:
            ppse = build_ppse(apps)
            ppse_source = "derived"
            flags |= FLAG_PPSE_DERIVED
    else:
        ppse = build_ppse(apps)
        flags |= FLAG_PPSE_DERIVED

    if len(ppse) > MAX_FCI:
        raise ValueError(f"PPSE len {len(ppse)} > {MAX_FCI}")
    if pse_fci and len(pse_fci) > MAX_FCI:
        raise ValueError(f"PSE len {len(pse_fci)} > {MAX_FCI}")

    return Profile(
        ppse=ppse,
        pse=pse_fci,
        shared_records=shared,
        apps=apps,
        flags=flags,
        ppse_source=ppse_source,
    )


def summarize(p: Profile) -> str:
    lines = [
        f"apps={len(p.apps)} shared_rec={len(p.shared_records)} flags=0x{p.flags:02X} "
        f"PPSE={p.ppse_source}",
        f"ppse_len={len(p.ppse)} pse_len={len(p.pse)}",
    ]
    for i, a in enumerate(p.apps):
        lines.append(
            f"  [{i}] AID={a.aid.hex().upper()} AIP={a.aip.hex().upper()} "
            f"rec={len(a.records)} IMK={'yes' if a.flags & FLAG_HAS_IMK else 'no'} "
            f"ICC={'yes' if a.flags & FLAG_HAS_ICC else 'no'} "
            f"IAD={'yes' if a.flags & FLAG_HAS_IAD else 'no'} "
            f"prio={a.prio.hex().upper()}"
        )
        for r in a.records:
            lines.append(f"      SFI{r.sfi}/rec{r.rec} len={len(r.data)}")
    for r in p.shared_records:
        lines.append(f"  PSE SFI{r.sfi}/rec{r.rec} len={len(r.data)}")
    return "\n".join(lines)


def load_profile_bytes(xml_path: Path) -> Tuple[bytes, Profile]:
    p = parse_cardimage(xml_path)
    return pack_profile(p), p


def assert_offlinecam_goldens(prof: Profile) -> None:
    """Hard checks for OfflineCAM_04 Collis semantics."""
    ppse = prof.ppse
    if bytes.fromhex("8701019F2A0102") not in ppse:
        raise AssertionError(
            f"PPSE missing Kernel ID 9F2A after 87: {ppse.hex().upper()}"
        )
    fci = prof.apps[0].aid_fci
    if bytes.fromhex("9F4D020B64") not in fci:
        raise AssertionError(
            f"AID FCI missing LogEntry 9F4D020B64: {fci.hex().upper()}"
        )
    # SFI2/rec1 pads + SFI3/rec2 SSAD
    sfi2 = next((r for r in prof.apps[0].records if r.sfi == 2 and r.rec == 1), None)
    if not sfi2 or b"\x00\x00\x00\x00\x00\x00\x00" not in sfi2.data:
        raise AssertionError("SFI2/rec1 missing InjectedData zero pads after 57")
    sfi3r2 = next((r for r in prof.apps[0].records if r.sfi == 3 and r.rec == 2), None)
    if not sfi3r2 or bytes.fromhex("93") not in sfi3r2.data[:6]:
        # tag 93 with non-trivial length
        raise AssertionError("SFI3/rec2 missing tag 93 SSAD")
    # Find 93 length
    d = sfi3r2.data
    i = d.find(b"\x93")
    if i < 0 or i + 1 >= len(d):
        raise AssertionError("SFI3/rec2 no 93")
    ln = d[i + 1]
    if ln < 64:
        raise AssertionError(f"SFI3/rec2 SSAD too short len={ln}")


if __name__ == "__main__":
    import sys

    path = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).with_name("card.xml"))
    try:
        blob, prof = load_profile_bytes(path)
    except ValueError as e:
        print(f"REJECT: {e}")
        raise SystemExit(1)
    print(summarize(prof))
    print(f"profile_bytes={len(blob)}")
    if "OfflineCAM_04" in path.name:
        assert_offlinecam_goldens(prof)
        print("GOLDEN OfflineCAM_04 OK")
