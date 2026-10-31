// Alde-LIN-Bridge: WomoLIN LIN-Controller als LIN-Master am gelben CI-Bus
// einer Alde Compact 3030 Plus. Stellt den Zustand per Zeilenprotokoll über
// TCP (WLAN) und USB-Seriell bereit; der Cerbo-GX-Dienst verbindet sich per TCP.
// Die Weboberfläche auf Port 80 erlaubt den Betrieb auch ohne Cerbo.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "alde_protocol.h"
#include "config.h"
#include "lin_master.h"
#include "network.h"
#include "settings.h"
#include "web_ui.h"

static LinMaster lin(Serial1, PIN_LIN_RX, PIN_LIN_TX);
static WiFiServer server(TCP_PORT);
static WiFiClient client;
static WebServer web(HTTP_PORT);
static Settings cfg;
static uint32_t restartAt = 0;  // Neustart nach dem Speichern der Einstellungen

static alde::State current;
static bool linkUp = false;
static uint8_t failCount = 0;
static uint32_t lastPoll = 0, lastDiag = 0, lastPublish = 0;
static char lastLine[200];

static alde::Command pending;
static uint8_t attempts = 0, confirmPolls = 0;
static char lastError[64];  // letzter asynchroner Befehlsfehler, für die Webseite

struct LineReader {
  char buf[160];
  size_t len = 0;
  bool overflow = false;
};
static LineReader usbReader, tcpReader;

// ── Ausgabe ────────────────────────────────────────────────────────────────

static void emit(const char *line) {
  Serial.println(line);
  if (client && client.connected()) {
    client.print(line);
    client.print('\n');
  }
}

static void logf(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.print("# ");
  Serial.println(buf);
}

static void publishState(bool force) {
  char line[200];
  alde::formatState(linkUp ? &current : nullptr, line, sizeof line);
  if (force || strcmp(line, lastLine) != 0 || millis() - lastPublish >= STATE_HEARTBEAT_MS) {
    emit(line);
    strcpy(lastLine, line);
    lastPublish = millis();
  }
}

static void setLed(int pin, bool on) { digitalWrite(pin, on ? LOW : HIGH); }

// ── Befehle ────────────────────────────────────────────────────────────────

static void failPending(const char *reason) {
  char line[96];
  snprintf(line, sizeof line, "ERR %s", reason);
  emit(line);
  snprintf(lastError, sizeof lastError, "%s", reason);
  pending.clear();
}

// Nimmt "sp=21 gas=1 ..." an und reiht den Befehl ein (TCP, USB und Web).
static bool submitSet(const char *args, char *err, size_t errLen) {
  alde::Command cmd;
  if (!alde::parseSetArgs(args, cmd, err, errLen)) return false;
  if (!linkUp) {
    snprintf(err, errLen, "keine Verbindung zum Panel");
    return false;
  }
  pending.merge(cmd);
  attempts = 0;
  confirmPolls = 0;
  lastPoll = 0;  // sofort abarbeiten
  lastError[0] = '\0';
  return true;
}

static void handlePending() {
  if (alde::commandSatisfied(current, pending)) {
    logf("Befehl vom Panel übernommen");
    pending.clear();
    return;
  }
  if (confirmPolls > 0 && confirmPolls < COMMAND_CONFIRM_POLLS) {
    confirmPolls++;
    return;
  }
  if (attempts >= COMMAND_RETRIES) {
    failPending("Panel hat Befehl nicht uebernommen");
    return;
  }
  uint8_t frame[8];
  alde::buildControl(current, pending, frame);
  lin.sendControl(frame);
  attempts++;
  confirmPolls = 1;
  logf("TX Control %02X %02X %02X (Versuch %u)", frame[3], frame[4], frame[5], attempts);
}

// Rettungsweg über USB, falls die Bridge im WLAN nicht mehr erreichbar ist:
// "NET" zeigt die Betriebsart, "NET STA"/"NET AP" wechselt sie und startet neu.
static void handleNet(const char *arg) {
  if (!*arg) {
    Serial.printf("NET mode=%s ssid=%s ap=%s ip=%s apip=%s\n", cfg.apMode ? "ap" : "sta",
                  cfg.staSsid.c_str(), cfg.apSsid.c_str(), WiFi.localIP().toString().c_str(),
                  netApActive() ? WiFi.softAPIP().toString().c_str() : "-");
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
  } else if (!strcmp(line, "PING")) {
    emit("PONG");
  } else if (!strcmp(line, "GET")) {
    publishState(true);
  } else if (!strcmp(line, "DIAG")) {
    lin.sendDiag();
    emit("OK");
  } else if (!strncmp(line, "SET ", 4)) {
    char err[48];
    if (submitSet(line + 4, err, sizeof err)) {
      emit("OK");
    } else {
      char out[64];
      snprintf(out, sizeof out, "ERR %s", err);
      emit(out);
    }
  } else {
    emit("ERR unbekannter Befehl");
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

// ── LIN-Zyklus ─────────────────────────────────────────────────────────────

static void linCycle() {
  uint8_t data[8];
  if (lin.readInfo(data)) {
    alde::decodeInfo(data, current);
    if (!linkUp) logf("Panel antwortet");
    linkUp = true;
    failCount = 0;
    setLed(PIN_LED_LIN, true);
    if (pending.any()) handlePending();
    publishState(false);
    return;
  }

  setLed(PIN_LED_LIN, false);
  if (failCount < 255) failCount++;
  if (linkUp && failCount >= LINK_LOST_AFTER) {
    linkUp = false;
    logf("Panel antwortet nicht mehr");
    if (pending.any()) failPending("Verbindung zum Panel verloren");
  }
  if (!linkUp) {
    // Nach Neustart oder Spannungsausfall des Panels erneut anmelden.
    if (millis() - lastDiag >= 5000) {
      lin.sendDiag();
      lastDiag = millis();
    }
    publishState(false);
  }
}

// ── Netzwerk ───────────────────────────────────────────────────────────────

static void handleNetwork() {
  // An: im WLAN verbunden, blinkt: Access Point aktiv
  setLed(PIN_LED_NET, WiFi.status() == WL_CONNECTED || (netApActive() && millis() / 500 % 2));

  WiFiClient incoming = server.available();
  if (incoming) {
    if (client) client.stop();
    client = incoming;
    client.setNoDelay(true);
    tcpReader.len = 0;
    tcpReader.overflow = false;
    logf("TCP-Client verbunden: %s", client.remoteIP().toString().c_str());
    client.print("HELLO alde-bridge 1.0\n");
    publishState(true);
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
  char state[300], body[440], rssi[8] = "null";
  alde::formatJson(linkUp ? &current : nullptr, state, sizeof state);
  if (WiFi.status() == WL_CONNECTED) snprintf(rssi, sizeof rssi, "%d", (int)WiFi.RSSI());
  snprintf(body, sizeof body,
           "{\"state\":%s,\"pending\":%s,\"error\":\"%s\",\"rssi\":%s,\"ap\":%s}", state,
           pending.any() ? "true" : "false", lastError, rssi, netApActive() ? "true" : "false");
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", body);
}

static void webSet() {
  if (!webAuth()) return;
  // Formularfelder sp, gas, el, ww in die Syntax des Zeilenprotokolls übersetzen.
  char args[128] = "", err[48];
  size_t len = 0;
  for (const char *key : {"sp", "gas", "el", "ww"}) {
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
  if (!submitSet(args, err, sizeof err)) {
    sendJsonError(err);
    return;
  }
  logf("Web: SET %s", args);
  web.send(200, "application/json", "{\"ok\":true}");
}

// ── Einstellungen ──────────────────────────────────────────────────────────

static void webConfigGet() {
  if (!webAuth()) return;
  String j = "{\"mode\":";
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

  const bool restart = n.networkDiffers(cfg);
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
  pinMode(PIN_LED_LIN, OUTPUT);
  pinMode(PIN_LED_NET, OUTPUT);
  setLed(PIN_LED_LIN, false);
  setLed(PIN_LED_NET, false);

  lin.begin();
  logf("Alde-LIN-Bridge startet");
#ifdef ALDE_SIM
  logf("TESTBETRIEB: Panel wird simuliert (Antworten ueber den LIN-Transceiver)");
#endif

  settingsLoad(cfg);
  netBegin(cfg);

  MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  if (cfg.otaPass.length()) ArduinoOTA.setPassword(cfg.otaPass.c_str());
  ArduinoOTA.begin();
  server.begin();
  setupWeb();

  delay(100);
  lin.sendDiag();
  lastDiag = millis();
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

  uint32_t interval = pending.any() ? CONFIRM_INTERVAL_MS : POLL_INTERVAL_MS;
  if (lastPoll == 0 || millis() - lastPoll >= interval) {
    lastPoll = millis();
    linCycle();
  }
  delay(2);
}
