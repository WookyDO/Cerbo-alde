// Einstellungen der Bridge, gespeichert im NVS (Flash) und über die
// Setup-Seite bearbeitbar.
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

struct Settings {
  bool apMode = false;  // true: dauerhaft Access Point statt WLAN-Client
  String staSsid, staPass;
  String apSsid, apPass;  // apPass leer: Standardpasswort SETUP_AP_PASSWORD
  bool staticIp = false;
  IPAddress ip, gateway, subnet, dns;
  String otaPass, webPass;

  // false: noch nie eingerichtet, die Bridge startet im Setup-Modus.
  bool configured() const { return apMode || staSsid.length() > 0; }
};

void settingsLoad(Settings &s);
void settingsSave(const Settings &s);
