// Prüf-Firmware für die LIN-Ports der ESP32_LIN-Platine.
//
// Befehle über USB (je eine Zeile):
//   ID                  MAC-Adresse der Platine
//   SLAVE <port> <baud> Port als LIN-Slave betreiben
//   MASTER <port> <baud> <n>  n Frames in jede Richtung, danach RESULT
//   IDLE                Statistik als STATS ausgeben, alle Transceiver schlafen
//
// Ablauf eines Tests: ID_M2S trägt Daten vom Master zum Slave, auf ID_S2M
// antwortet der Slave. Die Nutzdaten folgen einem festen Muster, damit jede
// Seite Bitfehler selbst erkennt. Beide Seiten prüfen außerdem ihr TX-Echo.
#include <Arduino.h>

struct Port {
  int rx, tx, slp;
};
static const Port PORTS[] = {{-1, -1, -1}, {5, 6, 7}, {4, 10, 20}};  // Index = LIN-Port

static constexpr uint8_t ID_M2S = 0x10;
static constexpr uint8_t ID_S2M = 0x11;
static constexpr uint32_t FRAME_GAP_MS = 5;
static constexpr uint32_t FRAME_TIMEOUT_US = 30000;

enum class Role { Idle, Slave };
static Role role = Role::Idle;
static uint32_t rxOk, rxBad, rxTimeout, badPid, txCount, txEchoErr;
static int prevByte = -1;
static uint32_t lastRxMs = 0;
static uint8_t slaveSeq = 0;

static uint8_t pidOf(uint8_t id) {
  id &= 0x3F;
  const uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1;
  const uint8_t p1 = ~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5)) & 1;
  return id | (p0 << 6) | (p1 << 7);
}

static uint8_t checksum(uint8_t pid, const uint8_t *d) {
  uint16_t sum = pid;
  for (int i = 0; i < 8; i++) {
    sum += d[i];
    if (sum > 0xFF) sum -= 0xFF;
  }
  return ~uint8_t(sum);
}

static void fillPattern(uint8_t seq, uint8_t *d) {
  for (int i = 0; i < 8; i++) d[i] = seq + i * 37;
}

static bool patternOk(const uint8_t *d) {
  uint8_t expected[8];
  fillPattern(d[0], expected);
  return !memcmp(d, expected, 8);
}

static bool readN(uint8_t *b, size_t n, uint32_t timeoutUs) {
  const uint32_t start = micros();
  size_t got = 0;
  while (got < n) {
    if (Serial1.available())
      b[got++] = Serial1.read();
    else if (micros() - start > timeoutUs)
      return false;
  }
  return true;
}

// TJA1021: nach dem Einschalten Standby, in den Sleep Mode nur über Normal Mode.
static void sleepTransceiver(const Port &p) {
  pinMode(p.tx, OUTPUT);
  digitalWrite(p.tx, HIGH);
  pinMode(p.slp, OUTPUT);
  digitalWrite(p.slp, HIGH);
  delay(2);
  digitalWrite(p.slp, LOW);
}

static void stopAll() {
  Serial1.end();
  role = Role::Idle;
  sleepTransceiver(PORTS[1]);
  sleepTransceiver(PORTS[2]);
}

static void clearStats() {
  rxOk = rxBad = rxTimeout = badPid = txCount = txEchoErr = 0;
  prevByte = -1;
  lastRxMs = millis();
}

static bool startPort(int port, uint32_t baud) {
  if (port != 1 && port != 2) return false;
  stopAll();
  const Port &p = PORTS[port];
  Serial1.begin(baud, SERIAL_8N1, p.rx, p.tx);
  Serial1.setRxTimeout(1);
  digitalWrite(p.slp, HIGH);
  delay(2);
  while (Serial1.available()) Serial1.read();
  clearStats();
  return true;
}

// Break: 0x00 bei halber Baudrate = 18 Bitzeiten dominant.
static void sendHeader(uint8_t pid, uint32_t baud) {
  while (Serial1.available()) Serial1.read();
  Serial1.flush();
  Serial1.updateBaudRate(baud / 2);
  Serial1.write(uint8_t(0x00));
  Serial1.flush();
  Serial1.updateBaudRate(baud);
  const uint8_t h[2] = {0x55, pid};
  Serial1.write(h, 2);
  Serial1.flush();
}

// Echo des eigenen Headers suchen: nach Break kommen 0x55 und die PID.
static bool readHeaderEcho(uint8_t pid) {
  const uint32_t start = micros();
  while (micros() - start < FRAME_TIMEOUT_US) {
    if (!Serial1.available()) continue;
    if (Serial1.read() == 0x55) {
      uint8_t p;
      return readN(&p, 1, FRAME_TIMEOUT_US) && p == pid;
    }
  }
  return false;
}

static void runMaster(int port, uint32_t baud, uint32_t n) {
  if (!startPort(port, baud)) {
    Serial.println("ERR Port");
    return;
  }
  const uint8_t pidTx = pidOf(ID_M2S), pidRx = pidOf(ID_S2M);
  uint32_t echoErr = 0, respOk = 0, respTimeout = 0, respBad = 0, latMin = UINT32_MAX, latMax = 0;
  uint8_t d[9], echo[9];

  for (uint32_t i = 0; i < n; i++) {
    fillPattern(uint8_t(i), d);
    d[8] = checksum(pidTx, d);
    sendHeader(pidTx, baud);
    Serial1.write(d, 9);
    Serial1.flush();
    if (!readHeaderEcho(pidTx) || !readN(echo, 9, FRAME_TIMEOUT_US) || memcmp(echo, d, 9)) echoErr++;
    delay(FRAME_GAP_MS);

    sendHeader(pidRx, baud);
    const uint32_t t0 = micros();
    if (!readHeaderEcho(pidRx)) echoErr++;
    while (!Serial1.available() && micros() - t0 < FRAME_TIMEOUT_US) {
    }
    if (Serial1.available()) {
      const uint32_t lat = micros() - t0;
      latMin = min(latMin, lat);
      latMax = max(latMax, lat);
    }
    if (!readN(d, 9, FRAME_TIMEOUT_US))
      respTimeout++;
    else if (d[8] == checksum(pidRx, d) && patternOk(d))
      respOk++;
    else
      respBad++;
    delay(FRAME_GAP_MS);
  }
  Serial.printf("RESULT sent=%u echoErr=%u respOk=%u respBad=%u respTimeout=%u latMinUs=%u latMaxUs=%u\n",
                (unsigned)n, (unsigned)echoErr, (unsigned)respOk, (unsigned)respBad,
                (unsigned)respTimeout, (unsigned)(latMin == UINT32_MAX ? 0 : latMin), (unsigned)latMax);
  stopAll();
}

static void slavePoll() {
  if (!Serial1.available()) return;
  const int c = Serial1.read();
  const uint32_t now = millis();
  const bool gap = now - lastRxMs >= 3;
  lastRxMs = now;
  if (c != 0x55 || !(prevByte == 0x00 || gap)) {
    prevByte = c;
    return;
  }
  prevByte = -1;

  uint8_t p;
  if (!readN(&p, 1, FRAME_TIMEOUT_US)) return;
  const uint8_t id = p & 0x3F;
  if (pidOf(id) != p) {
    badPid++;
    return;
  }
  uint8_t d[9], echo[9];
  if (id == ID_M2S) {
    if (!readN(d, 9, FRAME_TIMEOUT_US))
      rxTimeout++;
    else if (d[8] == checksum(p, d) && patternOk(d))
      rxOk++;
    else
      rxBad++;
  } else if (id == ID_S2M) {
    fillPattern(slaveSeq++, d);
    d[8] = checksum(p, d);
    Serial1.write(d, 9);
    Serial1.flush();
    txCount++;
    if (!readN(echo, 9, FRAME_TIMEOUT_US) || memcmp(echo, d, 9)) txEchoErr++;
  }
  lastRxMs = millis();
}

static void handleLine(char *line) {
  int port = 0;
  unsigned long baud = 0, n = 0;
  if (!strcmp(line, "ID")) {
    Serial.printf("ID %012llX\n", ESP.getEfuseMac());
  } else if (sscanf(line, "SLAVE %d %lu", &port, &baud) == 2) {
    if (startPort(port, baud)) {
      role = Role::Slave;
      Serial.println("OK");
    } else {
      Serial.println("ERR Port");
    }
  } else if (sscanf(line, "MASTER %d %lu %lu", &port, &baud, &n) == 3) {
    runMaster(port, baud, n);
  } else if (!strcmp(line, "IDLE")) {
    Serial.printf("STATS rxOk=%u rxBad=%u rxTimeout=%u badPid=%u tx=%u txEchoErr=%u\n", (unsigned)rxOk,
                  (unsigned)rxBad, (unsigned)rxTimeout, (unsigned)badPid, (unsigned)txCount,
                  (unsigned)txEchoErr);
    stopAll();
  } else {
    Serial.println("ERR Befehl");
  }
}

void setup() {
  Serial.begin(115200);
  stopAll();
}

void loop() {
  static char buf[48];
  static size_t len = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) {
        buf[len] = '\0';
        handleLine(buf);
        len = 0;
      }
    } else if (len < sizeof buf - 1) {
      buf[len++] = c;
    }
  }
  if (role == Role::Slave) slavePoll();
}
