// Truma Combi mit CP Plus: die Bridge gibt sich als iNet Box aus (LIN-Slave).
//
// Das CP Plus ist LIN-Master (9600 Baud). Es fragt mit PID 0x18, ob die iNet
// Box Änderungen hat, und tauscht Statuspuffer über den LIN-Transport-Layer
// (0x3C/0x3D) aus. Reine Logik ohne Arduino-Abhängigkeiten (pio test -e native).
//
// Protokollwissen: danielfett/inetbox.py (Beschreibung, kein übernommener Code),
// mc0110/inetbox2mqtt.
#pragma once

#include <cstddef>
#include <cstdint>

namespace truma {

constexpr uint32_t BAUD = 9600;
constexpr uint8_t ID_UPDATE = 0x18;       // CP Plus fragt nach Änderungen
constexpr uint8_t ID_MASTER_REQ = 0x3C;   // Transport-Layer Master -> Slave
constexpr uint8_t ID_SLAVE_RESP = 0x3D;   // Transport-Layer Slave -> Master
constexpr uint8_t NAD = 0x03;             // Knotenadresse der iNet Box
constexpr uint8_t NAD_BROADCAST = 0x7F;

constexpr uint8_t SID_ASSIGN_NAD = 0xB0;
constexpr uint8_t SID_READ_ID = 0xB2;
constexpr uint8_t SID_HEARTBEAT = 0xB9;
constexpr uint8_t SID_UPLOAD = 0xBA;      // iNet Box -> CP Plus (Befehl)
constexpr uint8_t SID_DOWNLOAD = 0xBB;    // CP Plus -> iNet Box (Status)

extern const uint8_t IDENTIFIER[4];       // Supplier 0x4617, Function 0x1F00
extern const uint8_t PREAMBLE[10];

constexpr size_t BUFFER_LEN = 38;         // Puffer im Transport-Layer, mit Füllbytes
constexpr size_t HEADER_LEN = 14;         // Präambel + Länge, ID, Zähler, Prüfsumme
constexpr uint8_t STATUS_LEN = 0x14, STATUS_ID = 0x33;    // vom CP Plus
constexpr uint8_t COMMAND_LEN = 0x0C, COMMAND_ID = 0x32;  // zum CP Plus
constexpr uint8_t COUNTER_ID = 0x0D;                      // setzt nur den Zähler

enum HeatingMode : uint8_t { MODE_OFF = 0, MODE_ECO = 1, MODE_BOOST = 10 };
enum EnergyMix : uint8_t { MIX_NONE = 0, MIX_GAS = 1, MIX_ELECTRIC = 2, MIX_BOTH = 3 };

constexpr float ROOM_MIN = 5.0f, ROOM_MAX = 30.0f;

uint8_t pid(uint8_t id);
bool pidParityOk(uint8_t pid);
// Prüfsumme wie im iNet-Box-Protokoll: Summe mod 0xFF, invertiert, 0xFF -> 0.
uint8_t checksum(const uint8_t *data, size_t len, uint8_t start = 0);
// LIN-Prüfsumme eines Frames: klassisch für 0x3C/0x3D, sonst mit PID.
uint8_t frameChecksum(uint8_t id, const uint8_t data[8]);

// Temperaturen als Kelvin × 10; 0, 0x0AAA und 0xAAAA bedeuten "kein Wert".
float decodeTemp(uint16_t raw);
uint16_t encodeTemp(float celsius);  // < 5 °C oder NAN -> 0

struct State {
  float roomTarget = 0;    // 0 = Heizung aus, sonst 5..30 °C
  float roomCurrent = 0;   // °C oder NAN
  float waterCurrent = 0;  // °C oder NAN
  uint8_t waterTarget = 0; // 0, 40 (Eco), 60 (heiß), 200 (Boost)
  uint8_t heatingMode = MODE_OFF;
  uint8_t energyMix = MIX_GAS;
  uint16_t elPower = 0;    // 0, 900, 1800 W
  uint8_t opStatus = 0;    // 0 aus, 1 Warnung, 4 startet, 5 an
  uint16_t errorCode = 0;
  uint8_t payload[STATUS_LEN] = {0};  // Rohdaten, unbekannte Bytes werden zurückgeschrieben
};

// Gewünschte Änderungen; nicht gesetzte Felder bleiben wie vom CP Plus gemeldet.
struct Command {
  bool hasRoom = false, hasMode = false, hasWater = false, hasMix = false, hasEl = false;
  float roomTarget = 0;
  uint8_t heatingMode = MODE_OFF, waterTarget = 0, energyMix = MIX_GAS;
  uint16_t elPower = 0;

  bool any() const { return hasRoom || hasMode || hasWater || hasMix || hasEl; }
  void clear() { *this = Command(); }
  void merge(const Command &o);
};

// Statuspuffer (Präambel + Kopf + 20 Byte) prüfen und auswerten.
bool decodeStatus(const uint8_t *buf, size_t len, State &out);
// Statuspuffer bauen (für das simulierte CP Plus und Tests).
void encodeStatus(const State &s, uint8_t counter, uint8_t out[BUFFER_LEN]);

// Befehlspuffer: aktueller Zustand + Änderungen, 38 Byte inkl. Füllbytes.
void encodeCommand(const State &current, const Command &cmd, uint8_t counter,
                   uint8_t out[BUFFER_LEN]);
// Befehlspuffer auswerten (für das simulierte CP Plus und Tests).
bool decodeCommand(const uint8_t *buf, size_t len, Command &out);

// Regeln des CP Plus: Raumtemperatur und Heizstufe gehören zusammen, ebenso
// Energieart und Elektroleistung. Ergänzt cmd um die abhängigen Felder.
void normalize(const State &current, float lastRoomTarget, Command &cmd);
bool commandSatisfied(const State &current, const Command &cmd);

// ── Zeilenprotokoll ──────────────────────────────────────────────────────────
// SET-Argumente: sp=0|5..30 mode=off|eco|boost ww=0|40|60|200 mix=gas|el|mix el=0|900|1800
bool parseSetArgs(const char *args, Command &out, char *err, size_t errLen);
size_t formatState(const State *s, char *out, size_t outLen);
size_t formatJson(const State *s, char *out, size_t outLen);

// ── iNet Box (Transport-Layer-Slave) ─────────────────────────────────────────
class InetBox {
 public:
  // Antwort auf PID 0x18: erstes Byte 0xFF = Befehl liegt bereit, 0xFE = nichts.
  void updateResponse(uint8_t out[8]) const;
  // Master-Request (PID 0x3C, 8 Datenbytes) verarbeiten.
  void onMasterRequest(const uint8_t d[8]);
  // Nächste Antwort für PID 0x3D; false, wenn keine ansteht.
  bool nextResponse(uint8_t out[8]);
  bool hasResponse() const { return respCount_ > 0; }

  bool hasStatus() const { return statusCount_ > 0; }
  uint32_t statusCount() const { return statusCount_; }
  const State &state() const { return state_; }

  // Befehl einreihen; das CP Plus holt ihn nach dem nächsten 0x18 ab.
  void queue(const Command &cmd) { pending_.merge(cmd); }
  bool commandPending() const { return pending_.any(); }
  uint32_t uploadCount() const { return uploadCount_; }
  void setCounter(uint8_t c) { counter_ = c; }

 private:
  void respond(const uint8_t r[8]);
  void handleSingle(uint8_t sid, const uint8_t *payload, size_t len);
  void handleBuffer();
  void queueUpload();

  State state_;
  Command pending_;
  uint8_t counter_ = 0;
  uint32_t statusCount_ = 0, uploadCount_ = 0;

  uint8_t resp_[8][8];
  uint8_t respHead_ = 0, respCount_ = 0;

  uint8_t rx_[64];
  size_t rxLen_ = 0, rxExpected_ = 0;
  bool rxActive_ = false;
};

// ── Simuliertes CP Plus (Master) für Tests und den Testbetrieb ───────────────
class CpPlusSim {
 public:
  // Ein Abfragezyklus: Anmeldung (einmal), 0x18, ggf. Befehl abholen, Status senden.
  void cycle(InetBox &box, float seconds);
  const State &panel() const { return panel_; }
  State &panel() { return panel_; }
  uint32_t commandsApplied() const { return applied_; }

 private:
  bool exchange(InetBox &box, const uint8_t req[8], uint8_t resp[8]);
  void step(float seconds);
  void sendStatus(InetBox &box);
  bool fetchCommand(InetBox &box);

  State panel_ = initialPanel();
  bool registered_ = false;
  bool heating_ = false;
  uint8_t counter_ = 0x40;
  uint32_t applied_ = 0;
  static State initialPanel();
};

}  // namespace truma
