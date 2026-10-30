// Alde-LIN-Bridge: WomoLIN LIN-Controller als LIN-Master am gelben CI-Bus
// einer Alde Compact 3030 Plus. Stellt den Zustand per Zeilenprotokoll über
// TCP (WLAN) und USB-Seriell bereit; der Cerbo-GX-Dienst verbindet sich per TCP.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "alde_protocol.h"
#include "config.h"
#include "lin_master.h"

static LinMaster lin(Serial1, PIN_LIN_RX, PIN_LIN_TX);
static WiFiServer server(TCP_PORT);
static WiFiClient client;

static alde::State current;
static bool linkUp = false;
static uint8_t failCount = 0;
static uint32_t lastPoll = 0, lastDiag = 0, lastPublish = 0;
static char lastLine[200];

static alde::Command pending;
static uint8_t attempts = 0, confirmPolls = 0;

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
  pending.clear();
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

static void handleLine(char *line) {
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
  if (!n) return;

  if (!strcmp(line, "PING")) {
    emit("PONG");
  } else if (!strcmp(line, "GET")) {
    publishState(true);
  } else if (!strcmp(line, "DIAG")) {
    lin.sendDiag();
    emit("OK");
  } else if (!strncmp(line, "SET ", 4)) {
    alde::Command cmd;
    char err[48];
    if (!alde::parseSetArgs(line + 4, cmd, err, sizeof err)) {
      char out[64];
      snprintf(out, sizeof out, "ERR %s", err);
      emit(out);
    } else if (!linkUp) {
      emit("ERR keine Verbindung zum Panel");
    } else {
      pending.merge(cmd);
      attempts = 0;
      confirmPolls = 0;
      lastPoll = 0;  // sofort abarbeiten
      emit("OK");
    }
  } else {
    emit("ERR unbekannter Befehl");
  }
}

static void readLines(Stream &in, LineReader &r) {
  while (in.available()) {
    char c = in.read();
    if (c == '\n') {
      r.buf[r.len] = '\0';
      if (!r.overflow) handleLine(r.buf);
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
  setLed(PIN_LED_NET, WiFi.status() == WL_CONNECTED);

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
  if (client && client.connected()) readLines(client, tcpReader);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_LIN, OUTPUT);
  pinMode(PIN_LED_NET, OUTPUT);
  setLed(PIN_LED_LIN, false);
  setLed(PIN_LED_NET, false);

  lin.begin();
  logf("Alde-LIN-Bridge startet");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
  server.begin();

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
  handleNetwork();
  readLines(Serial, usbReader);

  uint32_t interval = pending.any() ? CONFIRM_INTERVAL_MS : POLL_INTERVAL_MS;
  if (lastPoll == 0 || millis() - lastPoll >= interval) {
    lastPoll = millis();
    linCycle();
  }
  delay(2);
}
