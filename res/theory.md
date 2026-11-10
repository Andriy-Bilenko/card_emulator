# theory

Collis CardImage, FIME images - XML bank card images
PICC = Proximity Integrated Circuit Card (NFC contactless card)
Type/Kind A card image - contains Root `<CardImage>` + at least one `<Application>` (chip app data: AID, records, keys, etc.)
EMVCoL3CardImage / L3 scripts - scripted scenarios ("send this APDU, expect that"), not used in current project because we need to answer live SELECT/GPO/READ/GEN AC
ProfilePerso - pointer to another image and metadata
Magstripe images - no applications, magstripe-only tracks

magstripe track example: 
```
;36133400002108=491210100000999?
│     PAN      │YYMM│SSS│  DD  │
;             = 4912 101 00000999 ?

PAN   = Primary Account Number
4912  = expiry YYMM (Dec 2049)
101   = service code:
        1 = international use OK
        0 = normal authorization style (without online approval required)
        1 = PIN required, no restrictions like "ATM/goods-only"
00000999 = discretionary / issuer junk (could be CVV/CVC/PVV/etc.)
```

## Communication breakdown:

lowest layer is `[ ISO-14443-2 radio ] 13.56 MHz Type A/B (here A) modulation` (physical layer), modulated by PN532 NFC controller connected via SPI.

lowest programmable level:
example bytes we send over SPI from ESP32 to PN532:
```
01 00 00 FF 02 FE D4 02 2A 00

01 - SPI write marker
00 00 FF - frame start
02 -  length (D4 + DATA)
FE - LCS (Length Check Sum)
D4 - Host->PN532
02 - NXP command (Adafruit_PN532::getFirmwareVersion(void))
2A - DCS (Data Check Sum)
00 - Postamble
```
example bytes we receive over SPI:
```
00 00 FF 00 FF 00 - ACK

00 00 FF 06 FA D5 03 32 01 06 07 E8 00

00 00 FF - frame start
06 - length (D5 + response)
FA - LCS
D5 - PN532->Host
03 32 01 06 07 - NXP command response (response to GetFirmwareVersion, PN532 chip ID, firmware version 1.6 + support flags)
E8 - DCS
00 - Postamble
```
more in PN532 user manual: https://www.nxp.com/docs/en/user-guide/141520.pdf
(NXP commands in 7. Commands supported|Table 12. Command set, implemented in `Adafruit_PN532.h` which we use in current project)

functionality we use in current project:
- `GetFirmwareVersion()` - returns firmware bytes
- `TgInitAsTarget()` - makes PN532 act as contactless card waiting for activation from a reader up until entering `ISO-14443-4 (ISO-DEP)`
- `TgGetData()` - gets raw c-APDU bytes (PN532 handles ISO-DEP)
- `TgSetData()` - sends r-APDU bytes to PN532

after `ISO-14443-2` and before `ISO-14443-4 (ISO-DEP)` we have `ISO-14443-3` (fully covered by PN532 firmware after sending `TgInitAsTarget()`)

`ISO-14443-3` communication:
```
POS:   26                    REQA  (Request Type A)
  -or- 52                    WUPA  (Wake-Up Type A)
CARD:  04 00                 ATQA  (Answer To Request Type A)
POS:   93 20                 Anticollision
                             93 = cascade level 1 (pick UID at CL1)
                             20 = UID not known yet
CARD:  A2 3D 05 9A           UID + BCC
                             A2 3D 05 = UID
                             9A       = BCC (Block Check Character)
                                        = A2 ⊕ 3D ⊕ 05 ⊕ 00
POS:   93 70 A2 3D 05 9A     SELECT this UID
                             93 = CL1 (4-byte UID path)
                                  (95 = CL2 for 7/10-byte UIDs,
                                   97 = CL3 for 10-byte UIDs)
                             70 = NVB (Number of Valid Bits)
                                  “UID fully known - SELECT at CL1”
                             A2 3D 05 9A = UID + BCC
CARD:  20                    SAK  (Select Acknowledge)
```
RATS / ATS - openning ISO-14443-4:
```
POS:   E0 80                 RATS  (Request for Answer To Select)
                             E0 = RATS command
                             80 = FSDI + CID (FSDI=8 - reader max frame size = 256 bytes, CID=0)
CARD:  0D 52 46 49 44 49 4F 74 20 50 4E 35 33 32
                             ATS  (Answer To Select)
                             0D             = length 13
                             52 46 … 35 33 32 = ASCII "RFIDIOt PN532"
POS:   0A 00 A4 …            I-block + APDU (ISO-14443-4 / ISO-DEP started)
```

upon `RATS / ATS - open the smart channel` we get `[ ISO-14443-4 (ISO-DEP) blocks ] I-blocks / R-blocks / S-blocks (ISO-DEP)` in our communication. After ATS, FWT (Frame Waiting Time) applies - max time the POS waits for the card’s next RF reply

| Block | Job | example |
|-------|-----| ----- |
| I-block (Information) | Carries APDU bytes in INF |`0A 00 A4 04 00 0E 32 50 41 59 2E 53 59 53 2E 44 44 46 30 31 00`.    `0A` - PCB, everything else - INF (full c-APDU)|
| R-block (Receive-ready) | ACK / NAK (flow control) |`A2`. ACK|
| S-block (Supervisory) | Control, e.g. WTX = Wait Time extension |`F2 01`. `F2` - PCB wait request, `01` - WTX power multiplier (by which to multiply wait time)|

on every block PCB (Protocol Control Byte) is present, it mentions block type, chaining (MI = More Information), block number, whether CID follows, etc.

c-APDU/r-APDU - command and response APDUs
ISO/IEC-7816-4 is how we construct APDU (Application Protocol Data Unit)

c-APDU example:
```
00 | A4 | 04 | 00 | 07 | D2 76 00 00 85 01 01 | 00
CLA  INS  P1   P2   Lc            Data         Le
                 optional       optional    optional
```
- **CLA** - class (command family / protocol. `00` - default)
- **INS** - instruction (`A4` - Select. could be SELECT `A4`, READ RECORD `B2`, GET DATA `CA`, GET CHALLENGE `84`, VERIFY `20`, etc.)
- **P1/P2** - parameters how to select (`04` - by DF (dedicated file) name/AID, `00` - record №current record.) 
**P1** could be `00`- Select by file identifier, `01` - Select a child DF by file identifier, `02` - Select an EF (elementary file) under the current DF, `03` - Select the parent DF, `04` - Select by DF name / AID, etc.
**P2** could be `00` - also request File Control Information (FCI), `04` - also request File Control Parameters (FCP), `08` - req without FCI, etc.
- **Lc** - num. data bytes (7)
- **Data** - `D2 76 00 00 85 01 01` - AID
- **Le** - num. expected response bytes (`00` - up to 256, not '0')

r-APDU:
```
[ Response Data (optional) ] [ SW1 ] [ SW2 ]
```
- **SW1/2** - status bytes. could be `90 00` - Success, `6D 00` - Instruction not supported, `67 00` - wrong length, etc.

inside APDUs Data we have TLVs (Tag Length Value), TLVs can be nested

example:
```
simple:
5A 08 36 13 34 00 00 21 08 FF
│  │  └─ value X (8) bytes ─┘
│  └─ length = X bytes (8)
└─ tag 5A = PAN (Primary Account Number)

two-byte tag (two-byte if first five bits are set 0b00011111/0x1F):
5F 24 03 26 12 31
│  │  │  └─ YYMMDD expiry (2026-12-31)
│  │  └─ length 3
│  └─ second tag byte
└─ first → tag 5F24

long-length (over 127 bytes):
70 82 01 2C <300 bytes>
│  │    │
│  │    │ 
│  │    └─  2 bytes N = 0x012C = 300
│  └─ 1000 0010 = long-length marker (2 length bytes follow, 81 - 1 byte follows, etc.)
└─ constructed record template

constructed (boxes) (PPSE answer):
6F 2C                         FCI template, length 0x2C = 44 bytes inside
   84 0E 32 50 41 59 2E 53 59 53 2E 44 44 46 30 31
                             tag 84 = DF name, "2PAY.SYS.DDF01" (PPSE)
   A5 1A                      FCI proprietary template, 26 bytes inside
      BF 0C 17                tag BF0C = FCI issuer discretionary, 23 bytes inside
         61 15                tag 61 = Application Template, 21 bytes inside
            4F 07 A0 00 00 00 04 30 60
                             tag 4F = ADF name / AID (Maestro)
            50 07 4D 41 45 53 54 52 4F
                             tag 50 = Application Label "MAESTRO"
            87 01 01          tag 87 = Application Priority Indicator = 01
90 00                         SW success
```

EMV (Europay Mastercard Visa) - set of rules for contactless payments (which APDUs and TLV tags tap needs to use) (SELECT PPSE / SELECT AID / GEN AC / etc.)
could be useful to take a look at: 
https://mvallim.github.io/emv-qrcode/docs/EMV_v4.4_Book_3_Application_Specification.pdf
https://www.emvlab.org/emvtags/
https://www.emvlab.org/tlvutils/

### EMV behavioral tree:
#### 1. SELECT PPSE - Proximity Payment System Environment / contactless app menu (discovering AIDs on card)
POS request:
```
00 A4 04 00 0E 32 50 41 59 2E 53 59 53 2E 44 44 46 30 31 00
│  │  │  │  │  └──── "2PAY.SYS.DDF01" (14 bytes) ─────┘  │
│  │  │  │  └─ Lc = 14 bytes                             └─ Le
│  │  │  └─ P2 = 00 = also return FCI (File Control Information) 
│  │  └─ P1 = 04 = select by DF name / AID
│  └─ INS = A4 = SELECT
└─ CLA
```
card response:
```
6F 43                                              FCI template, len=67
   84 0E 32 50 41 59 2E 53 59 53 2E 44 44 46 30 31  DF (dedicated file) 14 bytes, ASCII "2PAY.SYS.DDF01" (required for PPSE)
   A5 31                                           FCI proprietary, len=49
      BF 0C 2E                                     PPSE directory, len=46
         61 18                                     app entry #1, len=24
            4F 07 A0 00 00 00 04 10 10             AID Mastercard (required)
            87 01 01                               priority = 1 (optional)
            50 0A 4D 61 73 74 65 72 63 61 72 64   label "Mastercard" (optional)
         61 12                                     app entry #2, len=18
            4F 07 A0 00 00 00 03 10 10             AID Visa
            87 01 02                               priority = 2
            50 04 56 69 73 61                       label "Visa"
90 00                                              success
```
#### 2. internally POS chooses AID from PPSE (POS reads tag `4F` (AID) list from PPSE FCI and picks one), here `4F 07 A0 00 00 00 04 10 10 Mastercard`
#### 3. SELECT AID = open the payment app chosen in step 2
   POS sends SELECT with that AID.
   Card either returns FCI (app open: DF name, optional label/PDOL/etc.) + `90 00`,
   or fails (e.g. `6A 82`) and then POS tries next AID / declines
POS request:
```
00 A4 04 00 07 A0 00 00 00 04 10 10 00
│  │  │  │  │  └──AID (7 bytes) ─┘  │
│  │  │  │  └─ Lc = 7               └─ Le
│  │  │  └─ P2 = 00 = return FCI
│  │  └─ P1 = 04 = select by name/AID
│  └─ INS = A4 = SELECT
└─ CLA = 00
```
card response example:
```
6F 50                                              FCI template, len=80
   84 07 A0 00 00 00 04 10 10                      DF name = selected AID (required)
   A5 45                                           FCI proprietary, len=69
      50 0F 50 50 43 20 4D 43 44 20 30 31 20 20 76 32 30
                                                   Application Label "PPC MCD 01  v20" (optional)
      87 01 01                                     Priority = 1 (optional)
      9F 38 18 9F 66 04 9F 02 06 9F 03 06 9F 1A 02 95 05 5F 2A 02 9A 03 9C 01 9F 37 04
                                             PDOL = Processing Data Object List (tag 9F38, optional, tag+length, no values)
                                             if present - step 4 packs those values into tag 83 (same order), if omitted/empty - step 4 uses 83 00
                                             9F66 04 = TTQ Terminal Transaction Qualifiers, what terminal can do, len 4
                                             9F02 06 = Amount Authorised, how much money, len 6
                                             9F03 06 = Amount Other, like cashback
                                             9F1A 02 = Terminal Country Code
                                             95 05   = TVR Terminal Verification Results, terminal flags
                                             5F2A 02 = Transaction Currency Code
                                             9A 03   = Transaction Date YYMMDD
                                             9C 01   = Transaction Type
                                             9F37 04 = UN Unpredictable Number, random anti-replay
      5F 2D 02 65 6E                               Language "en" (optional)
      9F 11 01 01                                  Issuer Code Table Index = 1 = Latin-1 (which character set issuer uses for names, optional)
      9F 12 0A 4D 61 73 74 65 72 63 61 72 64     Preferred name "Mastercard" (optional)
      BF 0C 0E 61 0C 4F 07 A0 00 00 00 04 10 10 87 01 01
                                                FCI Issuer Discretionary (tag BF0C, optional)
                                                61 0C = Application Template, 12 bytes
                                                4F 07 A0 00 00 00 04 10 10 = AID
                                                87 01 01 = priority 1
      9F 4D 02 0B 0A                               Log Entry SFI=0x0B (Short File Identifier, which files to read), records=0x0A (optional)
90 00                                              success
```
#### 4. Build and send request GPO (Get Processing Options) data from PDOL (Processing Options Data Object List)
POS request (if no PDOL):
```
80 A8 00 00 02 83 00 00
│  │  │  │  │  │  │  └─ Le
│  │  │  │  │  │  └─ 83 value length = 00 (no related data)
│  │  │  │  │  └─ tag 83 = PDOL related data
│  │  │  │  └─ Lc = 02
│  │  │  └─ P2 = 00
│  │  └─ P1 = 00
│  └─ INS = A8 = GET PROCESSING OPTIONS
└─ CLA = 80
```
POS request (if PDOL exists):
```
80 A8 00 00 23 83 21 A0 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 02 50 00 00 00 00 00 09 78 26 03 29 00 12 34 56 78 00
│  │  │  │  │  └──────────────── data ────────────────────────────────────────────────────────────────────────────────┘ └─ Le
│  │  │  │  └─ Lc = 23 hex = 35 = tag83 + len + 33 value bytes
│  │  │  └─ P2 = 00
│  │  └─ P1 = 00
│  └─ INS = A8
└─ CLA = 80

data breakdown:
83 21                         tag 83 (PDOL), length 21 hex = 33 bytes
   A0 00 00 00                9F66 TTQ = Terminal Transaction Qualifiers, what terminal can do "1010 0000"
   00 00 00 00 01 00          9F02 Amount Authorised 100
   00 00 00 00 00 00          9F03 Amount Other, no cashback
   02 50                      9F1A Terminal Country Code France
   00 00 00 00 00            95 TVR = Terminal Verification Results, no flags sent
   09 78                      5F2A Transaction Currency Code EUR
   26 03 29                   9A Transaction Date 2026-03-29
   00                         9C Transaction Type purchase
   12 34 56 78                9F37 UN = Unpredictable Number
```
card response:
```
77 16                                              Response Message Template Format 2 (tag 77, ), len 22
   82 02 19 80                                     AIP = Application Interchange Profile
                                                   card capability bit flags (ODA/CDA support etc.) `0x19` = `00011001` support for CVM, terminal risk management and CDA
   94 10 08 01 01 00 10 01 01 01 18 01 02 00 20 01 02 00
                                                   AFL = Application File Locator
                                                   four entries, 4 bytes each:
                                                      08 01 01 00 = SFI1, read 1 record, not for ODA
                                                      10 01 01 01 = SFI2, read 1 record, 1 record for ODA
                                                      18 01 02 00 = SFI3, read records 1 2, not for ODA
                                                      20 01 02 00 = SFI4, read records 1 2, not for ODA
                                                   SFI = Short File Identifier / file number for READ RECORD
90 00                                              success
```
- format 2 (tag 77) - nested TLVs inside, tag 82 = AIP, tag 94 = AFL
- format 1 (tag 80) - AIP bytes + AFL bytes stuck together in one blob, older format (can't have additional tags inside like format 2)

#### 5. READ RECORD
##### read #1 (SFI 1 record 1)
POS request:
```
00 B2 01 0C 00
│  │  │  │  └─ Le
│  │  │  └─ P2 = (1 << 3) | 04 = 08 | 04 = 0C (SFI 1)
│  │  └─ P1 = 01 = record number 1
│  └─ INS = B2 = READ RECORD
└─ CLA = 00
```
card response:
```
70 1C                                              record template, len 28
   9F 6C 02 00 01                                  Magstripe Application Version Number
   9F 62 00                                        Track1 bit map (empty value)
   9F 63 00                                        Track1 discretionary bit map (empty)
   56 00                                           Track1 data (empty)
   9F 64 00                                        Track1 ATC track (empty)
   9F 65 00                                        Track2 bit map (empty)
   9F 66 00                                        Track2 discretionary bit map (empty)
   9F 6B 00                                        Track2 data (empty)
   9F 67 00                                        Track2 ATC (empty)
90 00                                              success
```
##### read #2 (SFI 2 record 1)
POS request:
```
00 B2 01 14 00
│  │  │  │  └─ Le
│  │  │  └─ P2 = (2 << 3) | 04 = 10 | 04 = 14 (SFI 2)
│  │  └─ P1 = 01
│  └─ INS = B2
└─ CLA = 00
```
card response:
```
70 81 AD
   57 11 54 13 33 00 89 60 00 10 D3 01 22 01 01 23 40 91 72
                                                   Track2 Equivalent Data
   5A 08 54 13 33 00 89 60 00 10                   PAN
   5F 20 0C 45 54 45 43 2F 50 41 59 50 41 53 53   Cardholder Name "ETEC/PAYPASS"
   5F 24 03 30 12 31                               App Expiration Date 2030-12-31
   5F 25 03 04 01 01                               App Effective Date 2004-01-01
   5F 28 02 00 56                                  Issuer Country Code (0056 = France)
   5F 34 01 01                                     PAN Sequence Number 01
   8C 21 9F 02 06 9F 03 06 9F 1A 02 95 05 5F 2A 02 9A 03 9C 01 9F 37 04 9F 35 01 9F 45 02 9F 4C 08 9F 34 03                                     
                                                CDOL1 = Card Risk Management Data Object List 1, tag 8C, used for FIRST GENERATE AC:
                                                (tag+length, no values)
                                                   9F 02 06 = Amount Authorised
                                                   9F 03 06 = Amount Other
                                                   9F 1A 02 = Terminal Country Code
                                                   95 05    = TVR Terminal Verification Results (if anything failed already)
                                                   5F 2A 02 = Transaction Currency Code
                                                   9A 03    = Transaction Date YYMMDD
                                                   9C 01    = Transaction Type
                                                   9F 37 04 = UN Unpredictable Number
                                                   9F 35 01 = Terminal Type (attended/unattended, online/offline, etc.)
                                                   9F 45 02 = DAC (Data Authentication Code), for SDA
                                                   9F 4C 08 = ICC Dynamic Number (for DDA/CDA)
                                                   9F 34 03 = CVM (Cardholder Verification Method) Results (e.g. no CVM, PIN OK, PIN failed, signature, etc.)
   8D 0C 91 0A 8A 02 95 05 9F 37 04 9F 4C 08    CDOL2 = Card Risk Management Data Object List 2
                                                tag 8D, used for SECOND GENERATE AC
                                                   91 0A =    Issuer Authentication Data (From the bank after online request)
                                                   8A 02 =    Authorisation Response Code (approve/decline)
                                                   95 05 =    TVR again
                                                   9F 37 04 = UN again
                                                   9F 4C 08 = ICC Dynamic Number again
   8E 0E 00 00 00 00 00 00 00 00 42 03 5E 03 1F 03 CVM List (amount thresholds, if terminal supports ask for PIN/ask for signature/no CVM required)
   9F 07 02 FF 00                                  AUC Application Usage Control, where this app is allowed (ATM, goods, cashback, domestic/intl, etc.)
   9F 08 02 00 02                                  Application Version Number
   9F 0D 05 00 00 00 00 00                         IAC (Issuer Action Codes) Default
   9F 0E 05 00 00 00 00 00                         IAC Denial
   9F 0F 05 00 00 00 00 00                         IAC Online
   9F 42 02 09 78                                  Application Currency Code EUR
   9F 4A 01 82                                     SDA Tag List, extra tags to include in the SDA  hash (hash includes tag 82 AIP)
90 00
```
##### read #3 (SFI 3 record 1)
POS request:
```
00 B2 01 1C 00
│  │  │  │  └─ Le
│  │  │  └─ P2 = (3 << 3) | 04 = 18 | 04 = 1C (SFI3)
│  │  └─ P1 = 01
│  └─ INS = B2
└─ CLA = 00
```
card response:
```
70 81 E0                                           record template, len 224
   8F 01 F1
   └─ tag 8F = CA (Certification Authority) Public Key Index = F1 (to verify the issuer certificate)
   9F 32 01 03
   └─ tag 9F32 = Issuer RSA Public Key Exponent = 03 (to form issuer public key)
   92 24
      B0 4D A3 9B 98 05 C8 97 01 AE CA ED A7 62 60 F9 05
      D5 33 2D 7F 27 94 77 91 B7 26 71 E0 04 6B F3 64 DE 76 A5
   └─ Issuer Public Key Remainder, len 24
      leftover modulus bytes that did not fit inside the certificate field
   90 81 B0
      98 1B 6B B6 BB DC 75 70 C8 8F 79 3C 7F AF BF AB
      5F 31 2C 17 8F 81 2D EC D6 C5 62 26 F5 81 4D 84
      EA D9 D5 F1 90 B4 4D 20 7E C9 33 58 CF 3F C9 4C
      FB C2 DE 8A 89 C7 F7 4B 03 84 A8 2C 51 24 BC CB
      56 09 A9 E4 28 7C 63 C7 6E 6D C1 7D 72 CD A8 E3
      FE 00 A9 B8 FC A6 C3 6D A4 DE E5 EF 28 3A BF 53
      1E 35 98 A9 4B 91 F8 2D 87 68 D9 98 D7 63 30 66
      83 5B 5D 8E FB EE 38 FF 37 2A 00 CA EF 94 29 A9
      4E 65 29 57 B4 62 4A FC 79 93 21 9D 47 E2 47 B6
      74 30 88 6D C6 93 8E 0E 32 59 0C 10 3B EA C9 E2
      54 79 7A 23 E7 8A D6 2D 51 B1 6D 40 EE F8 5B 54
   └─ Issuer Public Key Certificate, len 176
90 00
```
CA Public Key Index + Issuer Public Key Certificate + Issuer Public Key Exponent + Issuer Public Key Remainder = Issuer Public Key, needed for SDA

##### read #4 (SFI 3 record 2)
POS request:
```
00 B2 02 1C 00
│  │  │  │  └─ Le
│  │  │  └─ P2 = (3 << 3) | 04 = 18 | 04 = 1C (SFI3)
│  │  └─ P1 = 02 = record 2
│  └─ INS = B2
└─ CLA = 00
```
card response:
```
70 81 B3
│    └─ len 179
└─ record template
   93 81 B0
   │    └─ len 176 bytes
   └─ tag 93 = SSAD = Signed Static Application Data (issuer private key over static hash)
      AF 20 D6 A2 74 0C 40 45 6F 75 F7 AB 59 05 BE 5E
      A1 7C 88 6A 7E 15 30 F6 AF 95 B8 38 30 35 C9 B9
      98 26 A9 2C 8C E8 7A BE 00 B2 81 A4 A4 4D 88 0E
      1B 23 E9 2B C3 1C 6A 22 D0 69 F3 1C 59 B9 5D 13
      EC 02 E7 E8 0E 37 16 F7 3A D9 6F D1 3E 20 84 91
      39 47 ED 4C B9 D4 75 5C 09 84 AF A7 E8 BC 02 56
      3C 39 2F 96 1B 23 0B 1A F7 37 F7 B0 50 A6 85 FF
      38 FD D0 4E B2 B9 D2 18 51 92 CD FF A4 42 28 67
      4B 94 CB 5A 9A 86 44 A9 C9 5E 97 09 EC 06 9A 4E
      58 14 21 5D 61 5F EB 39 CD DC 95 D8 0D 69 6F F8
      D0 98 5F 72 92 6D 07 8D A8 A2 58 6F 3F 77 47 6D
90 00
```
SSAD is needed for SDA in step 6

##### read #5 (SFI 4 record 1)
POS request:
```
00 B2 01 24 00
│  │  │  │  └─ Le
│  │  │  └─ P2 = (4 << 3) | 04 = 20 | 04 = 24 (SFI4)
│  │  └─ P1 = 01
│  └─ INS = B2
└─ CLA = 00
```
card response:
```
70 09
   9F 47 01 03                                     ICC Public Key Exponent
   9F 48 02 31 CD                                  ICC Public Key Remainder
90 00
```
##### read #6 (SFI 4 record 2)
POS request:
```
00 B2 02 24 00
│  │  │  │  └─ Le
│  │  │  └─ P2 = (4 << 3) | 04 = 20 | 04 = 24 (SFI4)
│  │  └─ P1 = 02
│  └─ INS = B2
└─ CLA = 00
```
card response:
```
70 81 B4
│    └─ len 180
└─ record template
   9F 46 81 B0
   │       └─ len 176
   └─ tag 9F46 = ICC Public Key Certificate
      1D 34 D8 D8 A5 56 C5 97 8D 6F A7 88 50 B3 98 63
      04 14 48 EF EC 21 69 1F 96 01 4F 0C EE F5 81 94
      4A 17 11 E8 EC 93 CC 11 A5 BD 5B 42 47 CF BD 7F
      3F E0 75 F1 D1 16 EE C3 7C B9 07 30 A8 65 D6 41
      D9 3D 51 E5 5B 11 00 A3 BC 3A 60 00 9A 12 1A F0
      B2 13 3B 81 0B 49 39 41 60 F1 76 53 68 D5 D7 2F
      D0 79 41 9E D7 74 88 D0 58 70 16 99 90 03 AB 81
      62 2C 8B EB 67 D8 C3 2D A3 F7 71 0E 95 65 05 F0
      94 F2 50 E6 C5 89 37 6C 58 B6 7E 9F C0 68 C8 94
      AC 65 F9 3D 83 73 B3 03 E7 2A EB CA 29 A1 0B A3
      59 EC D1 53 AE 34 D8 2A CC EC 98 92 69 2D D4 21
      └─ 176-byte issuer-signed certificate of the card public key
         POS: verify with issuer key → recover ICC modulus pieces
         combine with read #5 (9F47 exponent, 9F48 remainder)
         result = ICC public key for DDA/CDA signatures later
90 00
```

ICC Public Key Exponent + ICC Public Key Remainder + ICC Public Key Certificate = ICC (chip's) public key, needed for DDA/CDA


#### 6. Offline Data Authentication (ODA)
step for proving that data received from the bank card is genuine.
If AIP and terminal policy don't require any ODA step is skipped.

in current case we have CDA type of ODA (POS must support CDA).

types of ODA:
1. SDA (Static Data Authentication) - POS checking offline fixed card data signature signed by bank.
2. DDA (Dynamic Data Authentication) - proving live chip by signing a fresh challenge.
3. CDA (Combined DDA / AC Generation) - same as DDA + GENERATE AC.

type is being decided in AIP bytes during GPO and by whether POS supports it

##### SDA
1) POS decodes SSAD using issuer public key with RSA algorithm, we get 176 bytes:
```
6A 03 01                       recovered SSAD, algorithm SHA-1
[ DAC 2 bytes ]                here currently missing, sent in GEN AC (if found stored as 9F45 tag, else 00 00)
BB BB BB …                     padding
61 1A 91 6F 20 7D 45 25 97 A2 0F 2F 54 D3 8F 77 F8 EF 4F DC
└─ 20-byte (160 bit) SHA-1 hash the bank sealed
BC                                   trailer
```

2) POS builds static data:
during GPO we had:
```
10 01 01 01 = SFI2, read 1 record, 1 record for ODA
```
so we get all bytes of read #2 card response, because it has record for ODA

in read #2 we had: (mentions AIP)
```
9F 4A 01 82       SDA Tag List, extra tags to include in the SDA  hash (hash includes tag 82 AIP)
```
and in GPO we had 
```
82 02 19 80    AIP
```
so we get `19 80` AIP bytes

static data = [byte values inside tag 70 of read #2 card response starting with `57 11 ...` except final `90 00`] + `19 80`

3) POS compares
`SHA1(static_data)` must equal `hash_from_decoded_SSAD` for POS to trust static data, if not - SDA fails

##### DDA
POS request:
```
00 88 00 00 04 11 22 33 44 00
│  │  │  │  │  └──── challenge `11 22 33 44`, decided by DDOL (Dynamic Data Object List) if available
│  │  │  │  └─ Lc = 04
│  │  │  └─ P2 = 00
│  │  └─ P1 = 00
│  └─ INS = 88 = INTERNAL AUTHENTICATE
└─ CLA = 00
```
card response shape:
```
77 81 8C                     Response template Format 2, 140 bytes
   9F 4B 81 88               SDAD (Signed Dynamic Application Data), 136 bytes
      37 0B 14 A8 0E 00 A1 8C 11 03 B0 A2 95 C0 BD 8F
      A4 3E 5E 3D 4F CD 7B B9 4A F4 AA E0 5C 07 32 B9
      06 56 41 AC 6B BD 00 46 48 5A EC 3E 49 79 BE 78
      62 F1 68 04 4F 33 7D C2 74 CF C2 AF 8C 5B DA 41
      EB 30 57 B3 AB E2 F5 78 04 EF EE 44 47 54 62 30
      C6 B3 0B CA FE FE 83 1F 51 B1 5F 30 B3 43 0E 47
      61 7C C7 25 2D BB D4 73 39 95 61 D3 C1 9F 22 4E
      98 D6 18 78 CE D9 47 B5 3D 65 A5 3D 43 66 E8 49
      E6 01 57 C1 09 17 E9 7E
90 00
```
POS decodes value inside 9F4B tag with ICC/chip public key using RSA algorithm:
```
6A                                       header
05 01                                    SHA-1 algorithm
09                                       LDD = length of ICC Dynamic Data (LIDN+IDN)
08                                       LIDN = length of IDN
00 01 00 00 00 00 00 00                  IDN (ICC Dynamic Number) - here from ATC (Application Transaction Counter) 0001 + zeros, could be encrypted with 3DES algorithm and MKIDN (master key for IDN)
BB BB BB ...                             padding so body length = 114
68 6F 96 FD D6 1C D1 27 3E DB F8 7B 23 F7 B3 1A 29 9C 05 58     20-byte hash result (SHA-1(SDAD_body + challenge bytes))
BC                                       trailer
```
SDAD_body - decoded bytes from `05 01` to the last `BB` padding byte.

POS checks if SHA1(SDAD_body + challenge bytes) = decoded_hash_result, if so then DDA passes

##### CDA
AIP byte1 bit1 set means CDA supported
more explanations in step 8

#### 7. optional CVM / terminal risk
if during GPO in tag `82` (AIP) there is support for cardholder verification bit toggled then we should walk CVM List from read record read #2 card response if it exists and filled with data:
```
8E 0E 00 00 00 00 00 00 00 00 42 03 5E 03 1F 03 CVM List (Cardholder verification methods)
                  `00 00 00 00` - amount X (no amount checks)
                  `00 00 00 00` - amount Y (no amount checks)
                  `42 03` - rule #1, if terminal supoprts online PIN do online PIN, if fails try next rule
                  `5E 03` - if terminal supports then do signature verification, if fails try next rule
                  `1F 03` - if terminal supports no-CVM then no CVM required, else CVM failed
```
if offline PIN verify should run then POS sends APDU (not in our example):
```
00 20 00 80 08 24 12 34 FF FF FF FF FF 00
│  │  │  │  │  └ PIN block 8 bytes, format 2, 4 digits, `1234`
│  │  │  │  └─ Lc = 08
│  │  │  └─ P2 = 80 = plaintext PIN
│  │  └─ P1 = 00
│  └─ INS = 20 = VERIFY
└─ CLA = 00
```
card responds:
```
90 00              PIN OK
  -or-
69 83 / 63 Cx      Auth blocked / x tries left
```

POS should run TRM (terminal risk management) substep:
checking if in step 5 read #2 card is expired / not yet valid / allowed to use (AUC) / currency supported / any prior fails happened and visible in TVR bits

#### 8. First GENERATE AC (INS AE), build cryptogram from CDOL1
POS decides the request for cryptogram
Cryptogram types:
1. AAC (Application Authentication Cryptogram) - decline / abort
2. ARQC (Authorization Request Cryptogram) - ask online
3. TC (Transaction Certificate) - approved offline

if any set bits in TVR match IAC or TAC (Terminal Action Codes) Denial bits then AAC is chosen
else if any set bits in TVR match IAC or TAC (Terminal Action Codes) Online bits then ARQC is chosen
else if POS is allowed to approve offline and none of the set TVR bits are marked in Default policy as "do not approve offline" then TC is chosen
else ARQC is chosen for request from POS.
card still may answer differently (same or stricter option), it is up to specific issuer and it'd be a pretty hard time getting those rules from the card (if not impossible)

if AIP CDA supported and POS wants CDA then CDA is also used in this step

POS request (TC+CDA):
```
80 AE 50 00 2B 00 00 00 01 00 01 00 00 00 00 00 00 00 02 00 00 00 00 00 00 36 12 03 15 00 00 0E EC 85 22 00 00 00 00 00 00 00 00 00 00 5E 03 00 00
│  │  │  │  │  └──────────────── CDOL1 related values (43 bytes) ─────────────────────────────────────────────┘ 
│  │  │  │  └─ Lc = 2B hex = 43
│  │  │  └─ P2 = 00
│  │  └─ P1 = 50 = TC (40) + CDA request (10)
│  └─ INS = AE = GENERATE AC
└─ CLA = 80

CDOL1 value order (this capture's tag 8C):
   00 00 00 01 00 01     9F02 Amount Authorised
   00 00 00 00 00 00     9F03 Amount Other
   00 02                 9F09 Terminal Application Version Number
   00 00 00 00 00        95 TVR
   00 36                 5F2A Transaction Currency AUD
   12 03 15              9A Transaction Date 2012-03-15
   00                    9C Transaction Type purchase
   00 0E EC 85           9F37 Unpredictable Number
   22                    9F35 Terminal Type
   00 00                 9F45 Data Authentication Code (2 bytes from SDA)
   00 00 00 00 00 00 00 00     9F4C ICC Dynamic Number
   5E 03 00              9F34 CVM Results
```
card response:
```
77 81 B2                                           Response Message Template Format 2, len 178
   9F 27 01 40                                     CID = Cryptogram Information Data
                                                   40 = TC (00=AAC, 80=ARQC)
   9F 36 02 00 35                                  ATC = Application Transaction Counter = 53
   9F 4B 81 90                                     Signed Dynamic Application Data (CDA), len 144
      12 4C D6 88 98 FC 0F F1 B8 2E 60 33 A6 3D F0 3F
      66 01 DE D0 40 5B E0 DA 6A 55 7B A1 01 AE D9 56
      63 C6 EE 32 F8 95 F6 F2 C7 BB AC 56 8E 18 64 3C AE
      B9 76 2E 6C 24 84 3B 49 98 CC C8 F1 E6 08 A6 1C 9E
      19 14 31 5F E8 4D 56 35 0B 10 B3 69 4A EE CB 8B C3
      0A DA 3D B4 EC E3 31 D5 B7 E7 63 97 BD BA 77 63 14
      8F 53 5B 9E BF BA BE 77 2A B8 8C 7F 30 2F A9 E3 01
      0F 51 91 B7 34 E8 25 94 D0 87 52 EB B3 29 D7 6F BB
      F9 01 F7 79 92 1C E8 1C FB DC
   9F 10 12                                        Issuer Application Data, len 18
      01 10 90 40 09 24 84 00 00 00 00 00 00 00 00 00 29 FF
                                                   01 = key derivation index
                                                   10 = cryptogram version
                                                   90 40 09 24 84 00 = CVR / related bytes
                                                   00 00 - DAC (no SDA used)
                                                   00 00 00 00 00 00 29 FF = 8-byte counters
90 00
```

POS verifies CDA, first recover data in CDA (tag `9F4B`) using RSA algorithm and ICC/chip public key:
```
6A                                                                   header
05 01                                                                algorithm SHA-1
26                                                                   LDD
08                                                                   LIDN
36 DF 6D 9E 21 04 09 2E                                              IDN
40                                                                   CID, 40=TC
D5 8B 73 1A F5 88 5C 06                                              AC (Application Cryptogram)
7B E2 9D 01 5D D4 C9 45 40 26 81 0F 08 79 E2 19 B8 A7 DC D0          20-byte TDH (transaction data hash) - for securing transaction contents
BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB                                     padding
57 34 B6 2B E6 BF F2 A0 4C 1C FF 40 60 E5 49 C9 32 E1 72 3D          20-byte hash result
BC                                                                   trailer
```

IDN creation:
```
  blk[8] = 00 00 00 00 00 00 00 00
  blk[0] = ATC first byte = 00
  blk[1] = ATC second byte  = 35
  // blk = 00 35 00 00 00 00 00 00

  if personalized and MK-IDN (16-byte key) present:
      IDN = 3DES_encrypt(MK-IDN, blk)     // 8-byte out
  else:
      IDN = blk                           // 00 35 00 00 00 00 00 00
```
AC creation:
```
Y block = PAN without first and last digit + `0` + PSN digit
UDK (Unique DEA key) = encrypted Y with 3DES with IMK-AC + encrypted flipped Y with IMK-AC
SK (Session Key) = SK_left (<ATC 2 bytes> F0 00 00 00 00 00 encrypted with 3DES with UDK) + SK_right (<ATC 2 bytes> 0F 00 00 00 00 00 encrypted with 3DES with UDK)
take CDOL1 values with appended `80` and zeros until length is dividable by 8, split into 8-byte chunks, start with Y = `00 00 00 00 00 00 00 00`
for each chunk Y = DES_encrypt(SK_left, Y XOR chunk), 
decrypt Y with SK_right, encrypt again with SK_left, result is AC
```
TDH = SHA1(value inside GPO tag `83` (here empty)+ CDOL1 related values in GENERATE AC + GENERATE AC card response TLVs without CDA block)
hash result = SHA1(CDA body (from `05 01` to last `BB` padding byte) + UN `00 0E EC 85`)



POS computes hash result and compares to the one in recovered data, computes and compares TDH, compared CID. if any of them do not match - transaction offline decline
POS reads AC (it will be verified online)

#### 9. Go online (host authorization) - no card APDU
if step's 8 CID was ARQC then POS asks bank/issuer whether to approve

#### 10. EXTERNAL AUTHENTICATE (optional) - prove issuer answered
after step #9 (if there was an ARQC and host response)
step runs if AIP says issuer auth supported and host returned Issuer Auth Data and POS requests `00 82`, else skipped

POS request:
```
00 82 00 00 0A  AA BB CC DD EE FF 11 22  30 30  00
│  │  │  │  │   └────     ARPC    ────┘  └ARC┘  └─ Le
│  │  │  │  └─ Lc = 0A = 10
│  │  │  └─ P2 = 00
│  │  └─ P1 = 00
│  └─ INS = 82 = EXTERNAL AUTHENTICATE
└─ CLA = 00
```

card internally verifies that online answer really came from issuer:
```
ARPC = 3DES_encrypt(IMK-AC, ([<ARC> 00 00 00 00 00 00] XOR ARQC))
```

card response:
```
90 00        issuer auth accepted
 -or-
63 00        failed
 -or-
69 85        conditions not satisfied
```

#### 11. Second GENERATE AC - CDOL2 values
Same as step 8 but no ARQC option (only TC or AAC)
Only runs after step 8 returned ARQC and step 9 got a host answer.
Data list is CDOL2 (tag `8D`) instead of CDOL1
On this image, issuer-auth check (ARPC) also happens here inside tag `91` (if step 10 skipped)

POS request (host approved → ask TC):
```
80 AE 40 00 1D FF 27 D6 2A 9F 7E 1D 44 30 30 30 30 00 00 00 00 00 00 0E EC 85 36 DF 6D 9E 21 04 09 2E 00
│  │  │  │  │  └──────────────────── CDOL2 related values (29 bytes) ─────────────────────┘ 
│  │  │  │  └─ Lc 
│  │  │  └─ P2 = 00
│  │  └─ P1 = 40 = request TC   (00 = request AAC if host declined)
│  └─ INS = AE = GENERATE AC
└─ CLA = 80

CDOL2 value order (this image tag 8D):
   FF 27 D6 2A 9F 7E 1D 44 30 30     91 Issuer Authentication Data
                                      FF 27 D6 2A 9F 7E 1D 44 = ARPC
                                      30 30                   = ARC ASCII "00" approve
   30 30                           8A Authorisation Response Code ASCII "00" approve
   00 00 00 00 00                  95 TVR (no fail bits)
   00 0E EC 85                     9F37 Unpredictable Number (same as 1st GEN AC)
   36 DF 6D 9E 21 04 09 2E         9F4C ICC Dynamic Number (here = IDN from 1st CDA)
```

steps card does inside:
1. check ARPC in tag 91 (prove host answer is from issuer)
2. pick TC or AAC
3. build AC = same MAC as step 8, but over CDOL2 bytes
4. return CID + ATC + AC + IAD

card response:
```
77 29                                              Response Message Template Format 2, len 41
   9F 27 01 40                                     CID = Cryptogram Information Data
                                                   40 = TC   (00 = AAC decline)
   9F 36 02 00 35                                  ATC = Application Transaction Counter = 53
                                                   (same ATC as 1st GEN AC)
   9F 26 08 DC 3D D5 D1 2A DC BD 9B                AC = Application Cryptogram (MAC over CDOL2 bytes)
   9F 10 12                                        Issuer Application Data, len 18
      01 10 60 10 09 24 80 00 00 00 00 00 00 00 00 00 29 FF
                                                   01 = key derivation index
                                                   10 = cryptogram version
                                                   60 10 09 24 80 00 = CVR
                                                      60 = TC on 2nd + ARQC on 1st
                                                      10 = Issuer Authentication Performed
                                                      (rest same idea as step 8 IAD tail)
                                                   00 00 = DAC (none)
                                                   00 00 00 00 00 00 29 FF = counters
90 00                                
```


# TODO: where did I learn all this info from?
# TODO: use all pdf's available and notebooklm to verify if I got everything right



