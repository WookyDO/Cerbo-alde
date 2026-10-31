// Einstellungen der Bridge, gespeichert im NVS (Flash) und über die
// Setup-Seite bearbeitbar.
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

struct Settings {
  String heater = "alde";  // "alde" oder "truma"
  bool apMode = false;  // true: dauerhaft Access Point statt WLAN-Client
  String staSsid, staPass;
  String apSsid, apPass;  // apPass leer: Standardpasswort SETUP_AP_PASSWORD
  bool staticIp = false;
  IPAddress ip, gateway, subnet, dns;
  String otaPass, webPass;

  // false: noch nie eingerichtet, die Bridge startet im Setup-Modus.
  bool configured() const { return apMode || staSsid.length() > 0; }

  // true, wenn sich Heizungstyp, WLAN, IP oder Access Point unterscheiden (Neustart nötig).
  bool needsRestart(const Settings &o) const {
    return heater != o.heater || apMode != o.apMode || staSsid != o.staSsid ||
           staPass != o.staPass ||
           apSsid != o.apSsid || apPass != o.apPass || staticIp != o.staticIp ||
           (staticIp && (ip != o.ip || gateway != o.gateway || subnet != o.subnet ||
                         dns != o.dns));
  }
};

void settingsLoad(Settings &s);
void settingsSave(const Settings &s);
