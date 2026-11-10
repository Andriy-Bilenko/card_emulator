#pragma once
#include "ce_profile.h"

/**
 * @brief Run one ISO-DEP payment session as the loaded card
 *
 * After AsTarget: read C-APDUs, answer SELECT/GPO/READ/GEN AC/… from profile
 * (live AC/CDA/DDA when keys exist). Returns false if RF/TgSetData broke, else
 * true. Caller should end the AsTarget session
 */
bool ceEmulateSession(CeProfile* p);
