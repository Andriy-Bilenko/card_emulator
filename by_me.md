# TODOs:

- [ ] cleanup readme:
  - [X] explanation of the project
  - [X] setup and usage
    - [X] where to buy hardware, wire connections and soldering + photos
    - [X] linux environment setup
    - [X] compile and run instructions (including adafruit patch required)
  - [X] **maybe** features and limitations
  - [X] personal footer
  - [ ] finish TODOs
  - [ ] finish theory.md TODOs
- [X] add file comments that I am the author
- [X] remove `t6` from everywhere (it means "test6")
- [ ] add error logs or info logs everywhere so it is understandable if something fails
- [ ] FEIG payment terminal for some reason thinks our device differs from real card and is "non-payment" card, research and fix
- [ ] make logging actually usable and human readable
- [X] make device fail if pn532 is unable to initialise
- [ ] add how much out of how much blob takes to log message in bool t6ProfileSave(const T6Profile *p) `bool t6ProfileSave(const T6Profile *p)`
- [ ] use not only `SPIFFS.begin()` but also `SPIFFS.end()`
- [ ] print uint32_t v firmware version (all 4 bytes, not only 3, print support too)
- [ ] clean up adafruit patch
- [ ] remove hardcoded numbers and values from everywhere (especially 256 from t6NfcSetData)
- [ ] put "Andrii Bilenko" or any other owneship easter egg string into ATS during RATS
- [ ] try to extract all static and sensitive dynamic data from physical bank cards (to get a grasp how they work and possibly hack your own cards)
- [ ] if possible find out how (maybe using your own card dumps) to implement CIAC/CVR/etc. (and related CRM) (engine for choosing correct cryptogram AAC/ARQC/TC by bank card) to fully mimick a real bank card behavior
- [ ] make a full test copy of some 1-2-3 physical bank cards by running exhaustive tests on them and using your own keys inside, put into xml.
- [ ] clean up function comments in code, add comments to headers
- [ ] add more extensive logging so tester could see on which step transaction failed on the payment terminal, or which steps at all ran and what was inside
- [ ] add unit tests for everything

- [ ] figure out who owns the project (with papers) before disclosing the codebase
  1. ask if it is even needed for Wallee
  2. force my arguments for ownership:
    1. more that 100 hours of clean work from personal free time
    2. personal time (not company time), personal resources
    3. full code/device authorship
    4. personal spontaneous initiative (wanted to play with esp32 and nfc chips)
    5. unpaid, no jira tasks, no such project exists in Wallee
    6. no exclusive clause in contract

## not required TODOs:
- [ ] configure through web interface instead of USB by making a device standaone access point


# suggestions *
3 vectors of growing the project:
1. FIME xml card simulator
2. NFC APDUs retranslator   -   how about 2 same devices, one stuck to card, one to payment terminal, will they work as a relay together just transporting messages between card and payment terminal?
3. card back box simulator (reading bank card as a black box / simulating its responses to different scenarios)










# needed support for card images:
A. Mastercard family - Kernel C-2 (highest priority for test6)
ID	Flavor (XML description)	Typical AID / note
MC-01
MC credit contactless, CDA, offline-capable (your proven path)
A0000000041010 · e.g. PPC MCD 01
MC-02
MC credit CL, CDA, always-online / ARQC
same AID
MC-03
MC credit CL, DDA (INTERNAL AUTH), no CDA
AIP DDA
MC-04
MC credit CL, SDA only
AIP SDA
MC-05
MC credit CL, no ODA
AIP no ODA bits
MC-06
MC credit CL, No CVM floor
CVMList no-CVM
MC-07
MC credit CL, online PIN (high amount)
CVM online PIN
MC-08
MC credit CL, CDCVM / device CVM signalling
AIP/CVM for CDCVM
MC-09
MC debit CL (same kernel, debit AUC/IAC)
…1010 debit profile
MC-10
Maestro CL M/Chip, CDA or ARQC-online
A0000000043060 · MP*
MC-11
Maestro CL, forced online (CIAC-online style)
MP*
MC-12
Maestro CL, No CVM
MP*
MC-13
Cirrus / ATM-oriented if POS supports
A0000000046000
MC-14
US Maestro / US Debit variant
A0000000042203 · USM
MC-15
MC CL magstripe mode (CVC3 / MSD path)
MCM-style · test6 gap
MC-16
MC combo PPSE: Maestro + MC credit dual-app
two AIDs in PPSE
MC-17
MC with Kernel ID 9F2A in PPSE (InjectedData)
OfflineCAM-style
MC-18
MC expired app / cert (decline path)
past 5F24
MC-19
MC blocked / AAC-prefer risk
IAC/CIAC decline
MC-20
Offline CAM / CAM-focused subset card
OfflineCAM_04 class
MC-21
Long AID / issuer-specific ADF suffix
e.g. …1010D056…
MC-22
MC with empty IAD (omit 9F10)
image-accurate GEN AC
MC-23
MC 2nd GEN AC / CDOL2 relevant (if you ever extend firmware)
CDOL2 present
MC-24
MC contact twin of MC-01 (only if you add contact later)
optional

B. Visa - Kernel C-3
ID	Flavor	Note
VI-01
Visa credit CL qVSDC, fDDA/ODA as required
A0000000031010
VI-02
Visa credit CL, online ARQC / CVN 10/17/18 variants
different CVN
VI-03
Visa debit / Electron if in market
Electron AID
VI-04
Visa No CVM / CDCVM
VI-05
Visa online PIN
VI-06
Visa MSD / magstripe CL
often L3 or different image type
VI-07
Visa multi-app (credit + debit)
VI-08
Visa expired / blocked
VI-09
Visa with TTQ-sensitive GPO behaviour
qVSDC PDOL
(Many Visa CDET packs are EMVCoL3CardImage - need Kind‑A or a converter for test6.)

C. American Express - Kernel C-4
ID	Flavor
AX-01
Amex Expresspay credit CL, CDA/ODA as used in your region
AX-02
Amex CL online
AX-03
Amex No CVM / CDCVM
AX-04
Amex expired / decline

D. Discover / Diners - Kernel C-6 (+ Diners)
ID	Flavor
DI-01
Discover D-PAS CL
DI-02
Discover online / No CVM
DN-01
Diners CL (if distinct from Discover in your POS)

E. JCB - Kernel C-5
ID	Flavor
JC-01
JCB J/Speedy credit CL
JC-02
JCB debit CL (if applicable)
JC-03
JCB No CVM / online

F. UnionPay - Kernel C-7
ID	Flavor
UP-01
UnionPay QuickPass credit/debit CL
UP-02
UPI dual-app / multi-AID FCI
UP-03
UPI online / No CVM

G. Regional / domestic (fill only if POS accepts)
ID	Flavor	AID hint
IN-01
Interac CL
A000000277…
BA-01
BankAxept
NP-01
NAPAS
GI-01
Girocard / domestic DE (if any)
PF-01
PostFinance (ep2 brand)
ep2 kit
MC-CH
Maestro CH (ep2 brand)
ep2 kit
GP-01
Google Pay / tokenised profile (if you test tokens)
often not Kind‑A plastic twin
WX-01
Fleet / WEX
A000000768…
DNA-01
Debit Network Alliance

H. Cross-cutting “terminal stress” flavors (any scheme)
ID	Flavor
X-01
Multi-app PPSE (≥2 AIDs), priority order matters
X-02
Multi-app, terminal must pick non-default AID
X-03
Large READ records (near PN532 254 B limit)
X-04
Many records / deep AFL
X-05
Missing mandatory tag (faulty card → terminal decline/error)
X-06
Wrong CAPK index (8F) vs terminal keyset
X-07
Expired issuer/ICC cert
X-08
ATC near wrap / high ATC
X-09
Empty PDOL / minimal PDOL
X-10
Rich PDOL (amount, TTQ, etc.)
X-11
Language / currency exotic (5F2A / country)
X-12
Contactless only vs dual-interface ADF (CL tap still)




