#include "alde_protocol.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace alde {

const uint8_t DIAG_PAYLOAD[8] = {0x10, 0x06, 0xB2, 0x00, 0xDE, 0x41, 0x03, 0x00};

uint8_t pid(uint8_t id) {
  id &= 0x3F;
  uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1;
  uint8_t p1 = (~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5))) & 1;
  return id | (p0 << 6) | (p1 << 7);
}

uint8_t checksumEnhanced(uint8_t pid, const uint8_t *data, size_t len) {
  unsigned sum = pid;
  for (size_t i = 0; i < len; i++) {
    sum += data[i];
    if (sum > 0xFF) sum -= 0xFF;
  }
  return static_cast<uint8_t>(~sum & 0xFF);
}

float decodeTemp(uint8_t raw) {
  if (raw >= 0xFB) return NAN;
  return raw * 0.5f - 42.0f;
}

uint8_t encodeSetpoint(float celsius) {
  if (std::isnan(celsius)) celsius = SETPOINT_MIN;
  if (celsius < SETPOINT_MIN) celsius = SETPOINT_MIN;
  if (celsius > SETPOINT_MAX) celsius = SETPOINT_MAX;
  return static_cast<uint8_t>(std::lround((celsius - SETPOINT_MIN) * 2.0f));
}

float decodeSetpoint(uint8_t raw6) { return (raw6 & 0x3F) * 0.5f + SETPOINT_MIN; }

void decodeInfo(const uint8_t d[8], State &s) {
  memcpy(s.raw, d, 8);
  s.zone1 = decodeTemp(d[0]);
  s.zone2 = decodeTemp(d[1]);
  s.outdoor = decodeTemp(d[2]);
  s.setpoint = decodeSetpoint(d[3]);
  s.gas = (d[3] >> 6) & 1;
  s.valve = (d[3] >> 7) & 1;
  s.electric = (d[4] >> 6) & 3;
  s.panelOn = d[5] & 1;
  s.busy = (d[5] >> 1) & 1;
  s.error = (d[5] >> 2) & 1;
  uint8_t wm = (d[5] >> 3) & 3;
  s.water = (wm == 3) ? static_cast<uint8_t>(WATER_ON) : wm;  // 3 wird auf dem Bus nicht beobachtet
  s.ac = (d[5] >> 5) & 1;
  s.pump = (d[5] >> 7) & 1;
}

bool findInfoResponse(const uint8_t *buf, size_t len, uint8_t out[8]) {
  return findFrame(buf, len, pid(ID_INFO), out);
}

bool findFrame(const uint8_t *buf, size_t len, uint8_t p, uint8_t out[8]) {
  for (size_t i = 0; i + 11 <= len; i++) {
    if (buf[i] != 0x55 || buf[i + 1] != p) continue;
    const uint8_t *d = buf + i + 2;
    if (d[8] == checksumEnhanced(p, d, 8)) {
      memcpy(out, d, 8);
      return true;
    }
  }
  return false;
}

void Command::merge(const Command &o) {
  if (o.hasSetpoint) { hasSetpoint = true; setpoint = o.setpoint; }
  if (o.hasGas) { hasGas = true; gas = o.gas; }
  if (o.hasElectric) { hasElectric = true; electric = o.electric; }
  if (o.hasWater) { hasWater = true; water = o.water; }
}

void buildControl(const State &cur, const Command &cmd, uint8_t out[8]) {
  const uint8_t *r = cur.raw;
  uint8_t sp = cmd.hasSetpoint ? encodeSetpoint(cmd.setpoint) : (r[3] & 0x3F);
  bool gas = cmd.hasGas ? cmd.gas : cur.gas;
  uint8_t el = cmd.hasElectric ? (cmd.electric & 3) : cur.electric;

  out[0] = 0x00;
  out[1] = 0x00;
  out[2] = 0x00;
  // Bit 7 (Gasventil) wird unverändert zurückgegeben, wie vom Panel gemeldet.
  out[3] = sp | (gas ? 0x40 : 0) | (r[3] & 0x80);
  // Bits 0-5 (Zone 2) unverändert, Bits 6-7 Elektroleistung.
  out[4] = (r[4] & 0x3F) | (el << 6);
  // Statusbits unverändert, nur Warmwasser (Bits 3-4) ersetzen.
  out[5] = r[5];
  if (cmd.hasWater) out[5] = (r[5] & 0xE7) | ((cmd.water & 3) << 3);
  out[6] = 0xFF;
  out[7] = 0xFF;
}

bool commandSatisfied(const State &cur, const Command &cmd) {
  if (cmd.hasSetpoint && (cur.raw[3] & 0x3F) != encodeSetpoint(cmd.setpoint)) return false;
  if (cmd.hasGas && cur.gas != cmd.gas) return false;
  if (cmd.hasElectric && cur.electric != cmd.electric) return false;
  if (cmd.hasWater && cur.water != cmd.water) return false;
  return true;
}

// ── Zeilenprotokoll ─────────────────────────────────────────────────────────

static bool parseInt(const char *s, long lo, long hi, long &v) {
  char *end;
  v = strtol(s, &end, 10);
  return end != s && *end == '\0' && v >= lo && v <= hi;
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
    long v;
    if (!strcmp(key, "sp")) {
      char *end;
      float f = strtof(val, &end);
      if (end == val || *end != '\0' || !(f >= SETPOINT_MIN && f <= SETPOINT_MAX)) {
        snprintf(err, errLen, "sp ausserhalb 5..30");
        return false;
      }
      cmd.hasSetpoint = true;
      cmd.setpoint = f;
    } else if (!strcmp(key, "gas")) {
      if (!parseInt(val, 0, 1, v)) { snprintf(err, errLen, "gas 0|1"); return false; }
      cmd.hasGas = true;
      cmd.gas = v;
    } else if (!strcmp(key, "el")) {
      if (!parseInt(val, 0, 3, v)) { snprintf(err, errLen, "el 0..3"); return false; }
      cmd.hasElectric = true;
      cmd.electric = v;
    } else if (!strcmp(key, "ww")) {
      if (!parseInt(val, 0, 2, v)) { snprintf(err, errLen, "ww 0..2"); return false; }
      cmd.hasWater = true;
      cmd.water = v;
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

static void fmtTemp(char *out, size_t n, float t) {
  if (std::isnan(t))
    snprintf(out, n, "-");
  else
    snprintf(out, n, "%.1f", t);
}

size_t formatState(const State *s, char *out, size_t outLen) {
  if (!s) return snprintf(out, outLen, "STATE link=0");
  char z1[8], z2[8], ot[8];
  fmtTemp(z1, sizeof z1, s->zone1);
  fmtTemp(z2, sizeof z2, s->zone2);
  fmtTemp(ot, sizeof ot, s->outdoor);
  return snprintf(out, outLen,
                  "STATE link=1 z1=%s z2=%s out=%s sp=%.1f gas=%d valve=%d el=%u ww=%u "
                  "on=%d busy=%d err=%d ac=%d pump=%d raw=%02X%02X%02X",
                  z1, z2, ot, s->setpoint, s->gas, s->valve, s->electric, s->water,
                  s->panelOn, s->busy, s->error, s->ac, s->pump, s->raw[3], s->raw[4],
                  s->raw[5]);
}

}  // namespace alde
