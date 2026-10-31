// WLAN: Client mit Notfall-Access-Point, dauerhafter Access Point oder
// Setup-Modus (noch nicht eingerichtet).
#pragma once

#include "settings.h"

void netBegin(Settings &s);
void netLoop();
bool netApActive();
