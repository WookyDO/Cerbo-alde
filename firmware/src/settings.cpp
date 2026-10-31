#include "settings.h"

#include <Preferences.h>

#include "config.h"

// Interner Name; eine Änderung löscht die gespeicherten Einstellungen.
static constexpr const char *NS = "alde";

void settingsLoad(Settings &s) {
  Preferences p;
  p.begin(NS, true);
  s.heater = p.getString("heater", "alde");
  s.apMode = p.getBool("ap", false);
  s.staSsid = p.getString("ssid", "");
  s.staPass = p.getString("pass", "");
  s.apSsid = p.getString("apssid", SETUP_AP_SSID);
  s.apPass = p.getString("appass", "");
  s.staticIp = p.getBool("static", false);
  s.ip = IPAddress(p.getUInt("ip", 0));
  s.gateway = IPAddress(p.getUInt("gw", 0));
  s.subnet = IPAddress(p.getUInt("mask", uint32_t(IPAddress(255, 255, 255, 0))));
  s.dns = IPAddress(p.getUInt("dns", 0));
  s.otaPass = p.getString("ota", "");
  s.webPass = p.getString("web", "");
  p.end();
}

void settingsSave(const Settings &s) {
  Preferences p;
  p.begin(NS, false);
  p.putString("heater", s.heater);
  p.putBool("ap", s.apMode);
  p.putString("ssid", s.staSsid);
  p.putString("pass", s.staPass);
  p.putString("apssid", s.apSsid);
  p.putString("appass", s.apPass);
  p.putBool("static", s.staticIp);
  p.putUInt("ip", uint32_t(s.ip));
  p.putUInt("gw", uint32_t(s.gateway));
  p.putUInt("mask", uint32_t(s.subnet));
  p.putUInt("dns", uint32_t(s.dns));
  p.putString("ota", s.otaPass);
  p.putString("web", s.webPass);
  p.end();
}
