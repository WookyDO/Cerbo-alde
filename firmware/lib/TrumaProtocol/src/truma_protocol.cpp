#include "truma_protocol.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace truma {

const uint8_t IDENTIFIER[4] = {0x17, 0x46, 0x00, 0x1F};
const uint8_t PREAMBLE[10] = {0x00, 0x1E, 0x00, 0x00, 0x22, 0xFF, 0xFF, 0xFF, 0x54, 0x01};

uint8_t pid(uint8_t id) {
  id &= 0x3F;
  uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1;
  uint8_t p1 = (~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5))) & 1;
  return id | (p0 << 6) | (p1 << 7);
}

bool pidParityOk(uint8_t p) { return pid(p & 0x3F) == p; }

static unsigned sumMod(const uint8_t *data, size_t len, unsigned sum) {
  for (size_t i = 0; i < len; i++) sum = (sum + data[i]) % 0xFF;
  return sum;
}

static uint8_t finish(unsigned sum) {
  uint8_t cs = ~sum & 0xFF;
  return cs == 0xFF ? 0 : cs;
}

uint8_t checksum(const uint8_t *data, size_t len, uint8_t start) {
  return finish(sumMod(data, len, start));
}

uint8_t frameChecksum(uint8_t id, const uint8_t data[8]) {
  id &= 0x3F;
  if (id == ID_MASTER_REQ || id == ID_SLAVE_RESP) return checksum(data, 8);
  return checksum(data, 8, pid(id));
}

float decodeTemp(uint16_t raw) {
  if (raw == 0 || raw == 0x0AAA || raw == 0xAAAA) return NAN;
  return raw / 10.0f - 273.0f;
}

uint16_t encodeTemp(float c) {
  if (std::isnan(c) || c < ROOM_MIN) return 0;
  return static_cast<uint16_t>(std::lround((c + 273.0f) * 10.0f));
}

// Ist-Temperaturen dürfen auch unter 5 °C liegen.
static uint16_t encodeCurrent(float c) {
  if (std::isnan(c)) return 0;
  return static_cast<uint16_t>(std::lround((c + 273.0f) * 10.0f));
}

static uint8_t decodeWaterTarget(uint16_t raw) {
  float t = decodeTemp(raw);
  if (std::isnan(t)) return 0;
  if (t >= 100) return 200;
  if (t >= 50) return 60;
  if (t >= 30) return 40;
  return 0;
}

static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

void Command::merge(const Command &o) {
  if (o.hasRoom) { hasRoom = true; roomTarget = o.roomTarget; }
  if (o.hasMode) { hasMode = true; heatingMode = o.heatingMode; }
  if (o.hasWater) { hasWater = true; waterTarget = o.waterTarget; }
  if (o.hasMix) { hasMix = true; energyMix = o.energyMix; }
  if (o.hasEl) { hasEl = true; elPower = o.elPower; }
}

// Kopf prüfen: Präambel, Länge, ID und Prüfsumme über "54 01 len id zähler daten".
static bool checkBuffer(const uint8_t *buf, size_t len, uint8_t dataLen, uint8_t id) {
  if (len < HEADER_LEN + dataLen) return false;
  if (memcmp(buf, PREAMBLE, sizeof PREAMBLE) != 0) return false;
  if (buf[10] != dataLen || buf[11] != id) return false;
  unsigned sum = sumMod(buf + 8, 5, 0);
  sum = sumMod(buf + HEADER_LEN, dataLen, sum);
  return finish(sum) == buf[13];
}

static void finishBuffer(uint8_t out[BUFFER_LEN], uint8_t dataLen, uint8_t id, uint8_t counter) {
  memcpy(out, PREAMBLE, sizeof PREAMBLE);
  out[10] = dataLen;
  out[11] = id;
  out[12] = counter;
  unsigned sum = sumMod(out + 8, 5, 0);
  out[13] = finish(sumMod(out + HEADER_LEN, dataLen, sum));
  memset(out + HEADER_LEN + dataLen, 0, BUFFER_LEN - HEADER_LEN - dataLen);
}

// Nutzdaten des Statuspuffers (20 Byte), alle Werte Little Endian:
//  0 Soll Raum   2 Heizstufe   3 ?   4 Elektro W   6 Soll Wasser   8 Elektro W
// 10 Energie    11 Energie   12 Ist Wasser   14 Ist Raum   16 Betriebsstatus
// 17 Fehler (Byte 17 + 100 × Byte 18)   19 ?
bool decodeStatus(const uint8_t *buf, size_t len, State &s) {
  if (!checkBuffer(buf, len, STATUS_LEN, STATUS_ID)) return false;
  const uint8_t *p = buf + HEADER_LEN;
  memcpy(s.payload, p, STATUS_LEN);
  float room = decodeTemp(u16(p));
  s.roomTarget = std::isnan(room) ? 0 : room;
  s.heatingMode = p[2];
  s.elPower = u16(p + 4);
  s.waterTarget = decodeWaterTarget(u16(p + 6));
  s.energyMix = p[10];
  s.waterCurrent = decodeTemp(u16(p + 12));
  s.roomCurrent = decodeTemp(u16(p + 14));
  s.opStatus = p[16];
  s.errorCode = p[17] + 100 * p[18];
  return true;
}

void encodeStatus(const State &s, uint8_t counter, uint8_t out[BUFFER_LEN]) {
  uint8_t *p = out + HEADER_LEN;
  memcpy(p, s.payload, STATUS_LEN);
  put16(p, encodeTemp(s.roomTarget));
  p[2] = s.heatingMode;
  put16(p + 4, s.elPower);
  put16(p + 6, encodeTemp(s.waterTarget));
  put16(p + 8, s.elPower);
  p[10] = p[11] = s.energyMix;
  put16(p + 12, encodeCurrent(s.waterCurrent));
  put16(p + 14, encodeCurrent(s.roomCurrent));
  p[16] = s.opStatus;
  p[17] = s.errorCode % 100;
  p[18] = s.errorCode / 100;
  finishBuffer(out, STATUS_LEN, STATUS_ID, counter);
}

// Nutzdaten des Befehlspuffers (12 Byte): wie die ersten 12 Byte des Status.
void encodeCommand(const State &cur, const Command &cmd, uint8_t counter,
                   uint8_t out[BUFFER_LEN]) {
  const float room = cmd.hasRoom ? cmd.roomTarget : cur.roomTarget;
  const uint16_t el = cmd.hasEl ? cmd.elPower : cur.elPower;
  uint8_t *p = out + HEADER_LEN;
  put16(p, encodeTemp(room));
  p[2] = cmd.hasMode ? cmd.heatingMode : cur.heatingMode;
  p[3] = cur.payload[3];
  put16(p + 4, el);
  put16(p + 6, encodeTemp(cmd.hasWater ? cmd.waterTarget : cur.waterTarget));
  put16(p + 8, el);
  p[10] = p[11] = cmd.hasMix ? cmd.energyMix : cur.energyMix;
  finishBuffer(out, COMMAND_LEN, COMMAND_ID, counter);
}

bool decodeCommand(const uint8_t *buf, size_t len, Command &c) {
  if (!checkBuffer(buf, len, COMMAND_LEN, COMMAND_ID)) return false;
  const uint8_t *p = buf + HEADER_LEN;
  c.clear();
  c.hasRoom = c.hasMode = c.hasWater = c.hasMix = c.hasEl = true;
  float room = decodeTemp(u16(p));
  c.roomTarget = std::isnan(room) ? 0 : room;
  c.heatingMode = p[2];
  c.elPower = u16(p + 4);
  c.waterTarget = decodeWaterTarget(u16(p + 6));
  c.energyMix = p[10];
  return true;
}

void normalize(const State &cur, float lastRoom, Command &cmd) {
  float room = cmd.hasRoom ? cmd.roomTarget : cur.roomTarget;
  uint8_t mode = cmd.hasMode ? cmd.heatingMode : cur.heatingMode;
  if (cmd.hasRoom) {
    if (room < ROOM_MIN) {
      room = 0;
      mode = MODE_OFF;
    } else if (mode == MODE_OFF) {
      mode = MODE_ECO;
    }
  } else if (cmd.hasMode) {
    if (mode == MODE_OFF)
      room = 0;
    else if (room < ROOM_MIN)
      room = lastRoom >= ROOM_MIN ? lastRoom : 20.0f;
  }
  if (cmd.hasRoom || cmd.hasMode) {
    cmd.hasRoom = cmd.hasMode = true;
    cmd.roomTarget = room;
    cmd.heatingMode = mode;
  }

  if (cmd.hasMix || cmd.hasEl) {
    uint8_t mix = cmd.hasMix ? cmd.energyMix : cur.energyMix;
    uint16_t el = cmd.hasEl ? cmd.elPower : cur.elPower;
    if (cmd.hasEl && !cmd.hasMix) {
      if (el == 0)
        mix = MIX_GAS;
      else if (mix != MIX_ELECTRIC)
        mix = MIX_BOTH;
    }
    if (mix != MIX_ELECTRIC && mix != MIX_BOTH) {
      mix = MIX_GAS;
      el = 0;
    } else if (el == 0) {
      el = 900;
    }
    cmd.hasMix = cmd.hasEl = true;
    cmd.energyMix = mix;
    cmd.elPower = el;
  }
}

bool commandSatisfied(const State &cur, const Command &cmd) {
  if (cmd.hasRoom && std::fabs(cur.roomTarget - cmd.roomTarget) > 0.05f) return false;
  if (cmd.hasMode && cur.heatingMode != cmd.heatingMode) return false;
  if (cmd.hasWater && cur.waterTarget != cmd.waterTarget) return false;
  if (cmd.hasMix && cur.energyMix != cmd.energyMix) return false;
  if (cmd.hasEl && cur.elPower != cmd.elPower) return false;
  return true;
}

// ── Zeilenprotokoll ─────────────────────────────────────────────────────────

static const char *modeName(uint8_t m) {
  return m == MODE_ECO ? "eco" : m == MODE_BOOST ? "boost" : m == MODE_OFF ? "off" : "?";
}

static const char *mixName(uint8_t m) {
  return m == MIX_GAS ? "gas" : m == MIX_ELECTRIC ? "el" : m == MIX_BOTH ? "mix" : "none";
}

bool parseSetArgs(const char *args, Command &out, char *err, size_t errLen) {
  Command cmd;
  char buf[128];
  if (strlen(args) >= sizeof(buf)) {
    snprintf(err, errLen, "zu lang");
    return false;
  }
  strcpy(buf, args);

  for (char *tok = strtok(buf, " "); tok; tok = strtok(nullptr, " ")) {
    char *eq = strchr(tok, '=');
    if (!eq) {
      snprintf(err, errLen, "erwartet key=wert: %s", tok);
      return false;
    }
    *eq = '\0';
    const char *key = tok, *val = eq + 1;
    char *end;
    if (!strcmp(key, "sp")) {
      float f = strtof(val, &end);
      if (end == val || *end || !(f == 0 || (f >= ROOM_MIN && f <= ROOM_MAX))) {
        snprintf(err, errLen, "sp 0 oder 5..30");
        return false;
      }
      cmd.hasRoom = true;
      cmd.roomTarget = std::round(f);
    } else if (!strcmp(key, "mode")) {
      cmd.hasMode = true;
      if (!strcmp(val, "off")) cmd.heatingMode = MODE_OFF;
      else if (!strcmp(val, "eco")) cmd.heatingMode = MODE_ECO;
      else if (!strcmp(val, "boost")) cmd.heatingMode = MODE_BOOST;
      else { snprintf(err, errLen, "mode off|eco|boost"); return false; }
    } else if (!strcmp(key, "ww")) {
      long v = strtol(val, &end, 10);
      if (end == val || *end || (v != 0 && v != 40 && v != 60 && v != 200)) {
        snprintf(err, errLen, "ww 0|40|60|200");
        return false;
      }
      cmd.hasWater = true;
      cmd.waterTarget = v;
    } else if (!strcmp(key, "mix")) {
      cmd.hasMix = true;
      if (!strcmp(val, "gas")) cmd.energyMix = MIX_GAS;
      else if (!strcmp(val, "el")) cmd.energyMix = MIX_ELECTRIC;
      else if (!strcmp(val, "mix")) cmd.energyMix = MIX_BOTH;
      else { snprintf(err, errLen, "mix gas|el|mix"); return false; }
    } else if (!strcmp(key, "el")) {
      long v = strtol(val, &end, 10);
      if (end == val || *end || (v != 0 && v != 900 && v != 1800)) {
        snprintf(err, errLen, "el 0|900|1800");
        return false;
      }
      cmd.hasEl = true;
      cmd.elPower = v;
    } else {
      snprintf(err, errLen, "unbekannt: %s", key);
      return false;
    }
  }
  if (!cmd.any()) {
    snprintf(err, errLen, "keine Werte");
    return false;
  }
  out = cmd;
  return true;
}

static void fmtTemp(char *out, size_t n, float t, const char *none) {
  if (std::isnan(t))
    snprintf(out, n, "%s", none);
  else
    snprintf(out, n, "%.1f", t);
}

size_t formatState(const State *s, char *out, size_t outLen) {
  if (!s) return snprintf(out, outLen, "STATE link=0");
  char room[8], water[8];
  fmtTemp(room, sizeof room, s->roomCurrent, "-");
  fmtTemp(water, sizeof water, s->waterCurrent, "-");
  return snprintf(out, outLen,
                  "STATE link=1 room=%s sp=%.1f mode=%s water=%s ww=%u mix=%s el=%u "
                  "status=%u err=%u",
                  room, s->roomTarget, modeName(s->heatingMode), water, s->waterTarget,
                  mixName(s->energyMix), s->elPower, s->opStatus, s->errorCode);
}

size_t formatJson(const State *s, char *out, size_t outLen) {
  if (!s) return snprintf(out, outLen, "null");
  char room[8], water[8];
  fmtTemp(room, sizeof room, s->roomCurrent, "null");
  fmtTemp(water, sizeof water, s->waterCurrent, "null");
  return snprintf(out, outLen,
                  "{\"room\":%s,\"sp\":%.1f,\"mode\":\"%s\",\"water\":%s,\"ww\":%u,"
                  "\"mix\":\"%s\",\"el\":%u,\"status\":%u,\"err\":%u}",
                  room, s->roomTarget, modeName(s->heatingMode), water, s->waterTarget,
                  mixName(s->energyMix), s->elPower, s->opStatus, s->errorCode);
}

// ── iNet Box ────────────────────────────────────────────────────────────────

void InetBox::updateResponse(uint8_t out[8]) const {
  memset(out, 0xFF, 8);
  // Ohne bekannten Status lässt sich kein Befehlspuffer bauen.
  out[0] = (pending_.any() && statusCount_ > 0) ? 0xFF : 0xFE;
}

void InetBox::respond(const uint8_t r[8]) {
  if (respCount_ >= 8) return;
  memcpy(resp_[(respHead_ + respCount_) % 8], r, 8);
  respCount_++;
}

bool InetBox::nextResponse(uint8_t out[8]) {
  if (!respCount_) return false;
  memcpy(out, resp_[respHead_], 8);
  respHead_ = (respHead_ + 1) % 8;
  respCount_--;
  return true;
}

void InetBox::onMasterRequest(const uint8_t d[8]) {
  if (d[0] != NAD && d[0] != NAD_BROADCAST) return;
  const uint8_t pci = d[1] >> 4;
  if (pci == 0) {  // Single Frame
    const uint8_t len = d[1] & 0x0F;
    if (len < 1 || len > 6) return;
    rxActive_ = false;
    respCount_ = 0;  // neue Anfrage verwirft alte Antworten
    handleSingle(d[2], d + 3, len - 1);
  } else if (pci == 1) {  // First Frame
    const size_t total = ((d[1] & 0x0F) << 8) | d[2];
    respCount_ = 0;
    rxActive_ = false;
    if (d[3] != SID_DOWNLOAD || d[4] != 0x00 || d[5] != 0x1F || total < 5) return;
    rxExpected_ = total - 3;  // ohne SID und "00 1F"
    if (rxExpected_ > sizeof rx_) return;
    rx_[0] = d[6];
    rx_[1] = d[7];
    rxLen_ = 2;
    rxActive_ = true;
  } else if (pci == 2 && rxActive_) {  // Consecutive Frame
    for (int i = 2; i < 8 && rxLen_ < rxExpected_; i++) rx_[rxLen_++] = d[i];
    if (rxLen_ >= rxExpected_) {
      rxActive_ = false;
      handleBuffer();
    }
  }
}

void InetBox::handleSingle(uint8_t sid, const uint8_t *p, size_t len) {
  if (sid == SID_READ_ID && len >= 5 && p[0] == 0x00 && !memcmp(p + 1, IDENTIFIER, 4)) {
    const uint8_t r[8] = {NAD, 0x06, 0xF2, 0x17, 0x46, 0x00, 0x1F, 0x00};
    respond(r);
  } else if (sid == SID_ASSIGN_NAD && len >= 5 && !memcmp(p, IDENTIFIER, 4) && p[4] == NAD) {
    const uint8_t r[8] = {NAD, 0x01, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    respond(r);
  } else if (sid == SID_HEARTBEAT && len >= 2 && p[0] == 0x00 && p[1] == 0x1F) {
    const uint8_t r[8] = {NAD, 0x02, 0xF9, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    respond(r);
  } else if (sid == SID_UPLOAD && pending_.any() && statusCount_ > 0) {
    queueUpload();
  }
}

void InetBox::handleBuffer() {
  if (rxLen_ > 13 && rx_[11] == STATUS_ID) {
    State s;
    if (decodeStatus(rx_, rxLen_, s)) {
      state_ = s;
      statusCount_++;
    }
  } else if (rxLen_ > 12 && rx_[11] == COUNTER_ID) {
    counter_ = rx_[12];
  }
  const uint8_t r[8] = {NAD, 0x01, 0xFB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  respond(r);
}

// Befehl als First Frame + 6 Consecutive Frames (38 Byte Puffer).
void InetBox::queueUpload() {
  uint8_t buf[BUFFER_LEN];
  counter_ = (counter_ + 1) % 0xFF;
  encodeCommand(state_, pending_, counter_, buf);
  const uint8_t first[8] = {NAD, 0x10, 3 + BUFFER_LEN, 0xFA, 0x00, 0x1F, buf[0], buf[1]};
  respond(first);
  for (int i = 0; i < 6; i++) {
    uint8_t cf[8] = {NAD, uint8_t(0x21 + i)};
    memcpy(cf + 2, buf + 2 + 6 * i, 6);
    respond(cf);
  }
  pending_.clear();
  uploadCount_++;
}

// ── Simuliertes CP Plus ─────────────────────────────────────────────────────

State CpPlusSim::initialPanel() {
  State s;
  s.roomCurrent = 15.0f;
  s.waterCurrent = 20.0f;
  return s;
}

bool CpPlusSim::exchange(InetBox &box, const uint8_t req[8], uint8_t resp[8]) {
  box.onMasterRequest(req);
  return box.nextResponse(resp);
}

void CpPlusSim::cycle(InetBox &box, float seconds) {
  uint8_t r[8];
  if (!registered_) {
    const uint8_t readId[8] = {NAD_BROADCAST, 0x06, SID_READ_ID, 0x00, 0x17, 0x46, 0x00, 0x1F};
    if (!exchange(box, readId, r) || r[2] != 0xF2) return;
    const uint8_t assign[8] = {NAD_BROADCAST, 0x06, SID_ASSIGN_NAD, 0x17, 0x46, 0x00, 0x1F, NAD};
    if (!exchange(box, assign, r) || r[2] != 0xF0) return;
    registered_ = true;
  }
  const uint8_t heartbeat[8] = {NAD, 0x03, SID_HEARTBEAT, 0x00, 0x1F, 0xFF, 0xFF, 0xFF};
  exchange(box, heartbeat, r);

  step(seconds);
  uint8_t update[8];
  box.updateResponse(update);
  if (update[0] == 0xFF) fetchCommand(box);
  sendStatus(box);
}

bool CpPlusSim::fetchCommand(InetBox &box) {
  const uint8_t req[8] = {NAD, 0x03, SID_UPLOAD, 0x00, 0x1F, 0xFF, 0xFF, 0xFF};
  box.onMasterRequest(req);
  uint8_t r[8], buf[BUFFER_LEN];
  if (!box.nextResponse(r) || (r[1] >> 4) != 1 || r[3] != 0xFA) return false;
  size_t len = 0;
  buf[len++] = r[6];
  buf[len++] = r[7];
  while (len < BUFFER_LEN && box.nextResponse(r)) {
    if ((r[1] >> 4) != 2) return false;
    for (int i = 2; i < 8 && len < BUFFER_LEN; i++) buf[len++] = r[i];
  }
  Command c;
  if (!decodeCommand(buf, len, c)) return false;
  panel_.roomTarget = c.roomTarget;
  panel_.heatingMode = c.heatingMode;
  panel_.waterTarget = c.waterTarget;
  panel_.energyMix = c.energyMix;
  panel_.elPower = c.elPower;
  applied_++;
  return true;
}

void CpPlusSim::sendStatus(InetBox &box) {
  uint8_t buf[BUFFER_LEN];
  encodeStatus(panel_, ++counter_, buf);
  const uint8_t first[8] = {NAD, 0x10, 3 + BUFFER_LEN, SID_DOWNLOAD, 0x00, 0x1F, buf[0], buf[1]};
  box.onMasterRequest(first);
  for (int i = 0; i < 6; i++) {
    uint8_t cf[8] = {NAD, uint8_t(0x21 + i)};
    memcpy(cf + 2, buf + 2 + 6 * i, 6);
    box.onMasterRequest(cf);
  }
  uint8_t r[8];
  box.nextResponse(r);  // 0xFB
}

// Raum: Zweipunktregler mit 0,5 °C Hysterese, Wasser heizt Richtung Soll.
void CpPlusSim::step(float seconds) {
  State &p = panel_;
  const bool wants = p.heatingMode != MODE_OFF && p.roomTarget >= ROOM_MIN;
  if (!wants || p.roomCurrent >= p.roomTarget)
    heating_ = false;
  else if (p.roomCurrent <= p.roomTarget - 0.5f)
    heating_ = true;
  if (heating_) {
    p.roomCurrent += (p.heatingMode == MODE_BOOST ? 0.1f : 0.05f) * seconds;
    if (p.roomCurrent > p.roomTarget) p.roomCurrent = p.roomTarget;
  } else {
    p.roomCurrent -= (p.roomCurrent - 8.0f) * 0.002f * seconds;
  }
  if (p.waterTarget > 0) {
    const float target = p.waterTarget == 200 ? 62.0f : p.waterTarget;
    if (p.waterCurrent < target) p.waterCurrent = std::fmin(target, p.waterCurrent + 0.1f * seconds);
  } else {
    p.waterCurrent -= (p.waterCurrent - 15.0f) * 0.001f * seconds;
  }
  p.opStatus = (heating_ || p.waterTarget > 0) ? 5 : 0;
}

}  // namespace truma
