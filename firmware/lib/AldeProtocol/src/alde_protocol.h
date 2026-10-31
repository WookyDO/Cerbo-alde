// Alde Compact 3030 Plus – CI-Bus (gelber RJ12) Protokoll.
//
// Reine Logik ohne Arduino-Abhängigkeiten, damit sie auf dem PC getestet
// werden kann (pio test -e native).
//
// Protokollwissen: daviddootson/alde_3030_plus (PROTOCOL.md), WomoLIN.
#pragma once

#include <cstddef>
#include <cstdint>

namespace alde {

// 6-Bit-Frame-IDs (die PID mit Paritätsbits liefert pid()).
constexpr uint8_t ID_CONTROL = 0x1A;  // Master -> Panel, PID 0x1A
constexpr uint8_t ID_INFO = 0x1B;     // Panel -> Master, PID 0x5B
constexpr uint8_t ID_DIAG = 0x3C;     // Master -> Panel, PID 0x3C

// Anmeldung: NAD 0x10, PCI 0x06, SID 0xB2, Supplier 0x41DE (Alde),
// Function 0x0003 (3030 Plus).
extern const uint8_t DIAG_PAYLOAD[8];

constexpr float SETPOINT_MIN = 5.0f;
constexpr float SETPOINT_MAX = 30.0f;

enum WaterMode : uint8_t { WATER_OFF = 0, WATER_ON = 1, WATER_BOOST = 2 };

uint8_t pid(uint8_t id);
uint8_t checksumEnhanced(uint8_t pid, const uint8_t *data, size_t len);

// Temperatur aus dem Info-Frame; 0xFB..0xFF (Sonderwerte) -> NAN.
float decodeTemp(uint8_t raw);
uint8_t encodeSetpoint(float celsius);  // begrenzt auf 5..30 °C, 0,5-°C-Raster
float decodeSetpoint(uint8_t raw6);

struct State {
  float zone1 = 0;    // °C oder NAN
  float zone2 = 0;    // °C oder NAN (0xFE = Zone nicht vorhanden)
  float outdoor = 0;  // °C oder NAN
  float setpoint = 0;
  bool gas = false;
  bool valve = false;
  uint8_t electric = 0;  // 0..3 kW
  uint8_t water = 0;     // WaterMode
  bool panelOn = false;
  bool busy = false;
  bool error = false;
  bool ac = false;
  bool pump = false;
  uint8_t raw[8] = {0};
};

// Wertet die 8 Datenbytes des Info-Frames aus.
void decodeInfo(const uint8_t data[8], State &out);

// Sucht im Empfangspuffer nach "0x55 <pid>" gefolgt von 8 Datenbytes +
// gültiger Enhanced-Checksumme. Liefert true und kopiert die Datenbytes nach out.
bool findFrame(const uint8_t *buf, size_t len, uint8_t pid, uint8_t out[8]);

// findFrame() für den Info-Frame (Echo + Antwort des Panels, PID 0x5B).
bool findInfoResponse(const uint8_t *buf, size_t len, uint8_t out[8]);

// Gewünschte Änderungen; nicht gesetzte Felder bleiben wie vom Panel gemeldet.
struct Command {
  bool hasSetpoint = false;
  float setpoint = 0;
  bool hasGas = false;
  bool gas = false;
  bool hasElectric = false;
  uint8_t electric = 0;
  bool hasWater = false;
  uint8_t water = 0;

  bool any() const { return hasSetpoint || hasGas || hasElectric || hasWater; }
  void clear() { *this = Command(); }
  void merge(const Command &o);
};

// Read-Modify-Write: Steuerframe aus aktuellem Panelzustand + Änderungen.
void buildControl(const State &current, const Command &cmd, uint8_t out[8]);

// true, wenn das Panel alle gewünschten Änderungen übernommen hat.
bool commandSatisfied(const State &current, const Command &cmd);

// ── Zeilenprotokoll zum Cerbo GX ──────────────────────────────────────────
// Eingehend:  "SET sp=21.5 gas=1 el=2 ww=0" (beliebige Teilmenge), "PING"
// Ausgehend:  "STATE link=1 z1=21.0 ..." / "STATE link=0", "OK", "ERR <text>"

// Wertet die Argumente hinter "SET " aus. Bei Fehler false + Text in err.
bool parseSetArgs(const char *args, Command &out, char *err, size_t errLen);

// Formatiert eine STATE-Zeile (ohne Zeilenende). Rückgabe: Länge.
size_t formatState(const State *state, char *out, size_t outLen);

}  // namespace alde
