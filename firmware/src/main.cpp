// LIN-Heizungs-Bridge: WomoLIN LIN-Controller oder ESP32_LIN-Platine an einer Alde Compact 3030 Plus
// (LIN-Master) oder einer Truma Combi mit CP Plus (als iNet Box, LIN-Slave).
// Stellt den Zustand per Zeilenprotokoll über TCP (WLAN) und USB-Seriell bereit;
// der Cerbo-GX-Dienst verbindet sich per TCP. Die Weboberfläche auf Port 80
// erlaubt den Betrieb auch ohne Cerbo.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "heater.h"
#include "io.h"
#include "network.h"
#include "settings.h"
#include "web_ui.h"

static constexpr const char *VERSION = "1.1";

static WiFiServer server(TCP_PORT);
static WiFiClient client;
static WebServer web(HTTP_PORT);
static Settings cfg;
static Heater *heater = nullptr;
static uint32_t restartAt = 0;  // Neustart nach dem Speichern der Einstellungen

struct LineReader {
  char buf[160];
  size_t len = 0;
  bool overflow = false;
};
static LineReader usbReader, tcpReader;

// ── Ausgabe ────────────────────────────────────────────────────────────────

void emitLine(const char *line) {
  Serial.println(line);
  if (client && client.connected()) {
    client.print(line);
    client.print('\n');
  }
}

void logf(const char *fmt, ...) {
  char buf[160] = "# ";
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + 2, sizeof buf - 2, fmt, ap);
  va_end(ap);
  emitLine(buf);
}

// LEDs sind auf beiden Platinen aktiv low.
static void writeLed(int pin, bool on) { digitalWrite(pin, on ? LOW : HIGH); }

#if defined(BOARD_ESP32_LIN)
static bool linLed = false;
void setLinLed(bool on) { linLed = on; }

// Eine LED für alles: an = Heizung verbunden und Netz da (WLAN-Client
// verbunden oder dauerhafter Access Point), blinkt = Setup- oder Notfall-AP
// offen, aus = keine Verbindung zur Heizung.
static void updateLeds() {
  const bool blink = netApActive() && !cfg.apMode;
  const bool netOk = WiFi.status() == WL_CONNECTED || (cfg.apMode && netApActive());
  writeLed(PIN_LED_STATUS, blink ? millis() / 500 % 2 : linLed && netOk);
}

// ── Bordspannung (VSENSE) ──────────────────────────────────────────────────

static float vin = NAN;  // geglättet, Volt; NAN bis zur ersten Messung

static void readVin() {
  static uint32_t last = 0;
  if (last && millis() - last < 200) return;
  last = millis();
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_VSENSE);
  const float v = mv / 8 / 1000.0f * VSENSE_RATIO + VSENSE_DIODE_DROP;
  vin = isnan(vin) ? v : vin + (v - vin) * 0.2f;
}

// Bordspannung als Text ("12.6"), "-" ohne Messung bzw. unter 1 V (nur USB).
static const char *vinText() {
  static char buf[8];
  if (isnan(vin) || vin < 1.0f) return "-";
  snprintf(buf, sizeof buf, "%.1f", vin);
  return buf;
}
#else
void setLinLed(bool on) { writeLed(PIN_LED_LIN, on); }

static void updateLeds() {
  // Netz-LED an: im WLAN verbunden, blinkt: Access Point aktiv
  writeLed(PIN_LED_NET, WiFi.status() == WL_CONNECTED || (netApActive() && millis() / 500 % 2));
}
#endif

// ── Befehle ────────────────────────────────────────────────────────────────

// Rettungsweg über USB, falls die Bridge im WLAN nicht mehr erreichbar ist:
// "NET" zeigt die Betriebsart, "NET STA"/"NET AP" wechselt sie und startet neu.
static void handleNet(const char *arg) {
  if (!*arg) {
    Serial.printf("NET mode=%s ssid=%s ap=%s ip=%s apip=%s heater=%s\n",
                  cfg.apMode ? "ap" : "sta", cfg.staSsid.c_str(), cfg.apSsid.c_str(),
                  WiFi.localIP().toString().c_str(),
                  netApActive() ? WiFi.softAPIP().toString().c_str() : "-", cfg.heater.c_str());
#if defined(BOARD_ESP32_LIN)
    Serial.printf("NET board=esp32_lin lin=%s vin=%s\n",
                  cfg.heater == "truma" ? "LIN1 (J2/J6)" : "LIN2 (J5/J7, JP1 stecken)", vinText());
#endif
    return;
  }
  const bool ap = !strcmp(arg, "AP");
  if (!ap && strcmp(arg, "STA")) {
    Serial.println("ERR NET, NET STA oder NET AP");
    return;
  }
  if (!ap && cfg.staSsid.isEmpty()) {
    Serial.println("ERR kein WLAN gespeichert, Setup ueber den Access Point");
    return;
  }
  if (ap && cfg.apPass.length() < 8) {
    Serial.println("ERR kein eigenes AP-Passwort gespeichert");
    return;
  }
  cfg.apMode = ap;
  settingsSave(cfg);
  Serial.println("OK Neustart");
  restartAt = millis() + 500;
}

static void handleLine(char *line, bool usb) {
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
  if (!n) return;

  if (usb && (!strcmp(line, "NET") || !strncmp(line, "NET ", 4))) {
    handleNet(line[3] ? line + 4 : "");
#if defined(BOARD_ESP32_LIN)
  } else if (!strcmp(line, "VIN")) {
    char out[24];
    snprintf(out, sizeof out, "VIN %s", vinText());
    emitLine(out);
#endif
  } else if (!strcmp(line, "PING")) {
    emitLine("PONG");
  } else if (!strcmp(line, "GET")) {
    heater->publish(true);
  } else if (!strncmp(line, "SET ", 4)) {
    char err[48];
    if (heater->submit(line + 4, err, sizeof err)) {
      emitLine("OK");
    } else {
      char out[64];
      snprintf(out, sizeof out, "ERR %s", err);
      emitLine(out);
    }
  } else if (!heater->command(line)) {
    emitLine("ERR unbekannter Befehl");
  }
}

static void readLines(Stream &in, LineReader &r, bool usb) {
  while (in.available()) {
    char c = in.read();
    if (c == '\n') {
      r.buf[r.len] = '\0';
      if (!r.overflow) handleLine(r.buf, usb);
      r.len = 0;
      r.overflow = false;
    } else if (r.len < sizeof r.buf - 1) {
      r.buf[r.len++] = c;
    } else {
      r.overflow = true;
    }
  }
}

// ── Netzwerk ───────────────────────────────────────────────────────────────

static void handleNetwork() {
  updateLeds();

  WiFiClient incoming = server.available();
  if (incoming) {
    if (client) client.stop();
    client = incoming;
    client.setNoDelay(true);
    tcpReader.len = 0;
    tcpReader.overflow = false;
    logf("TCP-Client verbunden: %s", client.remoteIP().toString().c_str());
    client.printf("HELLO lin-bridge %s heater=%s\n", VERSION, heater->type());
    heater->publish(true);
  }
  if (client && client.connected()) readLines(client, tcpReader, false);
}

// ── Weboberfläche ──────────────────────────────────────────────────────────

static bool webAuth() {
  if (cfg.webPass.length() && !web.authenticate("admin", cfg.webPass.c_str())) {
    web.requestAuthentication();
    return false;
  }
  return true;
}

static String jsonStr(const String &s) {
  String out = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (uint8_t(c) < 0x20) {
      char esc[7];
      snprintf(esc, sizeof esc, "\\u%04x", c);
      out += esc;
    } else {
      out += c;
    }
  }
  return out + '"';
}

static void sendJsonError(const char *msg) {
  web.send(400, "application/json", String("{\"ok\":false,\"error\":") + jsonStr(msg) + "}");
}

static void redirect(const String &url) {
  web.sendHeader("Location", url);
  web.send(302, "text/plain", "");
}

static void webState() {
  if (!webAuth()) return;
  char state[300], rssi[8] = "null";
  heater->stateJson(state, sizeof state);
  if (WiFi.status() == WL_CONNECTED) snprintf(rssi, sizeof rssi, "%d", (int)WiFi.RSSI());
  String body = String("{\"type\":\"") + heater->type() + "\",\"state\":" + state +
#if defined(BOARD_ESP32_LIN)
                ",\"vin\":" + (strcmp(vinText(), "-") ? vinText() : "null") +
#endif
                ",\"pending\":" + (heater->pending() ? "true" : "false") +
                ",\"error\":" + jsonStr(heater->lastError()) + ",\"rssi\":" + rssi +
                ",\"ap\":" + (netApActive() ? "true" : "false") + "}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", body);
}

static void webSet() {
  if (!webAuth()) return;
  // Formularfelder in die Syntax des Zeilenprotokolls übersetzen; welche
  // Schlüssel gültig sind, prüft die jeweilige Heizung.
  char args[128] = "", err[48];
  size_t len = 0;
  for (const char *key : {"sp", "gas", "el", "ww", "mode", "mix"}) {
    if (!web.hasArg(key)) continue;
    const String value = web.arg(key);
    // Nur einfache Werte: kein zweiter Schlüssel, nichts, was das JSON bricht.
    if (value.isEmpty() || strpbrk(value.c_str(), " =\"\\") != nullptr) {
      sendJsonError("ungueltiger Wert");
      return;
    }
    len += snprintf(args + len, sizeof args - len, "%s%s=%s", len ? " " : "", key,
                    value.c_str());
    if (len >= sizeof args) {
      sendJsonError("zu lang");
      return;
    }
  }
  if (!heater->submit(args, err, sizeof err)) {
    sendJsonError(err);
    return;
  }
  logf("Web: SET %s", args);
  web.send(200, "application/json", "{\"ok\":true}");
}

// ── Einstellungen ──────────────────────────────────────────────────────────

static void webConfigGet() {
  if (!webAuth()) return;
  String j = "{\"heater\":" + jsonStr(cfg.heater) + ",\"mode\":";
  j += cfg.apMode ? "\"ap\"" : "\"sta\"";
  j += ",\"ssid\":" + jsonStr(cfg.staSsid);
  j += ",\"hasPass\":" + String(cfg.staPass.length() ? "true" : "false");
  j += ",\"apSsid\":" + jsonStr(cfg.apSsid);
  j += ",\"hasApPass\":" + String(cfg.apPass.length() ? "true" : "false");
  j += ",\"static\":" + String(cfg.staticIp ? "true" : "false");
  j += ",\"ip\":" + jsonStr(cfg.staticIp ? cfg.ip.toString() : "");
  j += ",\"gw\":" + jsonStr(cfg.staticIp ? cfg.gateway.toString() : "");
  j += ",\"mask\":" + jsonStr(cfg.subnet.toString());
  j += ",\"dns\":" + jsonStr(cfg.staticIp ? cfg.dns.toString() : "");
  j += ",\"hasOta\":" + String(cfg.otaPass.length() ? "true" : "false");
  j += ",\"hasWeb\":" + String(cfg.webPass.length() ? "true" : "false");
  j += ",\"configured\":" + String(cfg.configured() ? "true" : "false");
  j += ",\"staIp\":" + jsonStr(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "");
  j += ",\"apIp\":" + jsonStr(netApActive() ? WiFi.softAPIP().toString() : "");
  j += "}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

static bool parseIp(const char *key, IPAddress &out) {
  return out.fromString(web.arg(key)) && uint32_t(out) != 0;
}

// Prüft die neuen Einstellungen; nullptr wenn gültig, sonst die Fehlermeldung.
static const char *validate(const Settings &n) {
  if (n.heater != "alde" && n.heater != "truma") return "Heizungstyp unbekannt";
  if (!n.apMode && (n.staSsid.isEmpty() || n.staSsid.length() > 32))
    return "WLAN-Name: 1 bis 32 Zeichen";
  if (n.staPass.length() && (n.staPass.length() < 8 || n.staPass.length() > 63))
    return "WLAN-Passwort: 8 bis 63 Zeichen";
  if (n.apSsid.isEmpty() || n.apSsid.length() > 32) return "AP-Name: 1 bis 32 Zeichen";
  if (n.apPass.length() && (n.apPass.length() < 8 || n.apPass.length() > 63))
    return "AP-Passwort: 8 bis 63 Zeichen";
  if (n.apMode && n.apPass.isEmpty())
    return "Fuer den dauerhaften Access Point ein eigenes AP-Passwort setzen";
  if (n.otaPass.isEmpty()) return "OTA-Passwort fehlt (sonst keine Updates mehr moeglich)";
  if (!n.apMode && n.staticIp) {
    const uint32_t ip = n.ip, gw = n.gateway, mask = n.subnet;
    if ((ip & mask) != (gw & mask)) return "IP und Gateway liegen nicht im selben Netz";
    if (ip == gw) return "IP und Gateway sind gleich";
    if ((ip & ~mask) == 0 || (ip & ~mask) == ~mask) return "IP ist Netz- oder Broadcastadresse";
  }
  return nullptr;
}

static void webConfigPost() {
  if (!webAuth()) return;
  Settings n = cfg;
  if (web.hasArg("heater")) n.heater = web.arg("heater");
  n.apMode = web.arg("mode") == "ap";
  const String ssid = web.arg("ssid");
  // Neues Netz ohne Passwort: offenes WLAN; gleiches Netz: leer heißt unverändert.
  if (ssid != n.staSsid || web.arg("pass").length()) n.staPass = web.arg("pass");
  n.staSsid = ssid;
  n.apSsid = web.arg("apssid");
  if (web.arg("appass").length()) n.apPass = web.arg("appass");
  if (web.arg("ota").length()) n.otaPass = web.arg("ota");
  if (web.arg("webclear") == "1")
    n.webPass = "";
  else if (web.arg("web").length())
    n.webPass = web.arg("web");

  n.staticIp = web.arg("ipmode") == "static";
  if (!n.apMode && n.staticIp) {
    if (!parseIp("ip", n.ip) || !parseIp("gw", n.gateway) || !parseIp("mask", n.subnet)) {
      sendJsonError("IP, Gateway oder Subnetzmaske ungueltig");
      return;
    }
    if (web.arg("dns").isEmpty())
      n.dns = n.gateway;
    else if (!parseIp("dns", n.dns)) {
      sendJsonError("DNS ungueltig");
      return;
    }
  }

  if (const char *err = validate(n)) {
    sendJsonError(err);
    return;
  }

  const bool restart = n.needsRestart(cfg);
  const bool changed = restart || n.otaPass != cfg.otaPass || n.webPass != cfg.webPass;
  if (!changed) {
    web.send(200, "application/json",
             "{\"ok\":true,\"restart\":false,\"next\":\"Keine Aenderungen.\"}");
    return;
  }
  settingsSave(n);
  if (!restart) {
    // Passwörter gelten sofort, ohne Neustart.
    cfg.otaPass = n.otaPass;
    cfg.webPass = n.webPass;
    ArduinoOTA.setPassword(cfg.otaPass.c_str());
    logf("Passwoerter gespeichert");
    web.send(200, "application/json",
             "{\"ok\":true,\"restart\":false,\"next\":\"Gespeichert, gilt sofort.\"}");
    return;
  }

  // Wo die Bridge nach dem Neustart erreichbar ist; "url" fragt die Seite ab.
  String next, url;
  if (n.apMode) {
    next = "Mit dem WLAN \"" + n.apSsid + "\" verbinden, dann http://192.168.4.1 aufrufen.";
  } else if (n.staticIp) {
    url = "http://" + n.ip.toString() + "/";
  } else if (!cfg.apMode && cfg.staSsid == n.staSsid && WiFi.status() == WL_CONNECTED) {
    url = "http://" + WiFi.localIP().toString() + "/";  // DHCP vergibt meist dieselbe
  } else {
    next = String("Im WLAN \"") + n.staSsid + "\" unter http://" + HOSTNAME +
           ".local erreichbar (oder IP im Router nachsehen).";
  }
  if (url.length()) next = "Nach dem Neustart unter " + url + " erreichbar.";
  web.send(200, "application/json",
           "{\"ok\":true,\"restart\":true,\"next\":" + jsonStr(next) + ",\"url\":" +
               jsonStr(url) + "}");
  logf("Einstellungen gespeichert, Neustart");
  restartAt = millis() + 1500;
  if (!restartAt) restartAt = 1;
}

// WLAN-Suche läuft im Hintergrund; die Seite fragt nach, bis sie fertig ist.
static void webScan() {
  if (!webAuth()) return;
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    web.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (n < 0) {
    WiFi.scanNetworks(true);
    web.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  String j = "{\"scanning\":false,\"networks\":[";
  for (int i = 0; i < n && i < 20; i++) {
    if (WiFi.SSID(i).isEmpty()) continue;
    if (!j.endsWith("[")) j += ',';
    j += "{\"ssid\":" + jsonStr(WiFi.SSID(i)) + ",\"rssi\":" + String(WiFi.RSSI(i)) +
         ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  j += "]}";
  WiFi.scanDelete();
  web.send(200, "application/json", j);
}

static void setupWeb() {
  web.on("/", HTTP_GET, [] {
    if (!webAuth()) return;
    if (!cfg.configured())
      redirect("/setup");
    else
      web.send_P(200, "text/html; charset=utf-8", WEB_UI);
  });
  web.on("/setup", HTTP_GET, [] {
    if (webAuth()) web.send_P(200, "text/html; charset=utf-8", SETUP_UI);
  });
  web.on("/api/state", HTTP_GET, webState);
  web.on("/api/set", HTTP_POST, webSet);
  web.on("/api/config", HTTP_GET, webConfigGet);
  web.on("/api/config", HTTP_POST, webConfigPost);
  web.on("/api/scan", HTTP_GET, webScan);
  web.onNotFound([] {
    // Captive Portal: im AP-Modus alles auf die Startseite umleiten.
    if (netApActive())
      redirect("http://" + WiFi.softAPIP().toString() + "/");
    else
      web.send(404, "text/plain", "nicht gefunden");
  });
  web.begin();
  MDNS.addService("http", "tcp", HTTP_PORT);
}

void setup() {
  Serial.begin(115200);
#if defined(BOARD_ESP32_LIN)
  // USB-Seriell (USB-Serial/JTAG): ohne angeschlossenen Rechner nicht warten.
  Serial.setTxTimeoutMs(0);
  pinMode(PIN_LED_STATUS, OUTPUT);
  writeLed(PIN_LED_STATUS, false);
  // Beide Transceiver schlafen lassen; die Heizung weckt ihren in begin().
  for (int slp : {LIN_TRUMA.slp, LIN_ALDE.slp}) {
    pinMode(slp, OUTPUT);
    digitalWrite(slp, LOW);
  }
  analogSetPinAttenuation(PIN_VSENSE, ADC_11db);
#else
  pinMode(PIN_LED_LIN, OUTPUT);
  pinMode(PIN_LED_NET, OUTPUT);
  writeLed(PIN_LED_LIN, false);
  writeLed(PIN_LED_NET, false);
#endif

  settingsLoad(cfg);
  heater = createHeater(cfg.heater);
  logf("LIN-Heizungs-Bridge %s startet, Heizung: %s", VERSION, heater->type());
#if defined(BOARD_ESP32_LIN)
  logf("Platine ESP32_LIN, Heizung an %s",
       cfg.heater == "truma" ? "LIN1 (J2/J6)" : "LIN2 (J5/J7, Jumper JP1 = Master)");
#endif
#ifdef HEATER_SIM
  logf("TESTBETRIEB: Heizung wird simuliert");
#endif
  heater->begin();
  netBegin(cfg);

  MDNS.begin(HOSTNAME);
  // OTA erst nach der Einrichtung, nie ohne Passwort.
  if (cfg.otaPass.length()) {
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(cfg.otaPass.c_str());
    ArduinoOTA.begin();
  }
  server.begin();
  setupWeb();

}

void loop() {
  static wl_status_t lastWifi = WL_IDLE_STATUS;
  wl_status_t wifi = WiFi.status();
  if (wifi != lastWifi) {
    if (wifi == WL_CONNECTED) logf("WLAN verbunden, IP %s", WiFi.localIP().toString().c_str());
    lastWifi = wifi;
  }

  ArduinoOTA.handle();
  netLoop();
  handleNetwork();
  web.handleClient();
  readLines(Serial, usbReader, true);

  if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();

  heater->loop();
#if defined(BOARD_ESP32_LIN)
  readVin();
#endif
  delay(2);
}
