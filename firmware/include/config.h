// Hardware- und Netzwerkeinstellungen der LIN-Heizungs-Bridge.
// WLAN, IP und Passwörter werden auf der Setup-Seite eingestellt (settings.h).
#pragma once

#include <cstdint>

// Anschluss eines LIN-Transceivers; slp < 0: SLP fest verdrahtet.
struct LinPins {
  int rx, tx, slp;
};

#if defined(BOARD_ESP32_LIN)
// ESP32_LIN-Platine (ESP32-C3-WROOM-02, 2× TJA1021T/20)
//   LIN1 (J2/J6): fest Slave           -> Truma (Bridge = iNet Box)
//   LIN2 (J5/J7): Master mit JP1       -> Alde  (Bridge = LIN-Master)
constexpr LinPins LIN_TRUMA = {5, 6, 7};   // LIN1_RX, LIN1_TX, LIN1_SLP
constexpr LinPins LIN_ALDE = {4, 10, 20};  // LIN2_RX, LIN2_TX, LIN2_SLP
// Nur eine schaltbare LED (rot, D7 an GPIO8, aktiv low); die grüne ist die
// Power-LED. An: Heizung verbunden und Netz da, blinkt: Setup-/Notfall-AP offen.
constexpr int PIN_LED_STATUS = 8;
// Spannungsteiler 100k/15k an VIN_F, also hinter PTC und Verpolschutz-Diode.
constexpr int PIN_VSENSE = 3;
constexpr float VSENSE_RATIO = (100.0f + 15.0f) / 15.0f;
constexpr float VSENSE_DIODE_DROP = 0.3f;  // SS34 (D1) bei ~50 mA, zum Abgleich
#else
// WomoLIN LIN-Controller (Pinbelegung wie inetbox2mqtt, Profil "WOMOLIN"),
// ein Transceiver für beide Heizungen.
constexpr LinPins LIN_TRUMA = {14, 15, -1};
constexpr LinPins LIN_ALDE = {14, 15, -1};
constexpr int PIN_LED_LIN = 2;   // aktiv low
constexpr int PIN_LED_NET = 17;  // aktiv low
#endif

constexpr const char *HOSTNAME = "lin-bridge";
constexpr uint16_t TCP_PORT = 7020;
constexpr uint16_t HTTP_PORT = 80;

// Access Point für Ersteinrichtung und als Notfall-Zugang
constexpr const char *SETUP_AP_SSID = "LIN-Bridge";
constexpr const char *SETUP_AP_PASSWORD = "lin-setup";  // solange kein eigenes gesetzt ist
constexpr uint32_t FALLBACK_AP_AFTER_MS = 60000;  // WLAN so lange weg -> AP zusätzlich an
constexpr uint32_t STA_RETRY_MS = 30000;          // neuer Verbindungsversuch zum WLAN

constexpr uint32_t POLL_INTERVAL_MS = 500;      // normale Statusabfrage
constexpr uint32_t CONFIRM_INTERVAL_MS = 300;   // Abfrage nach einem Befehl
constexpr uint32_t STATE_HEARTBEAT_MS = 5000;   // STATE auch ohne Änderung
constexpr uint8_t LINK_LOST_AFTER = 6;          // Fehlversuche bis link=0
constexpr uint8_t COMMAND_RETRIES = 3;          // Sendeversuche pro Befehl
constexpr uint8_t COMMAND_CONFIRM_POLLS = 5;    // Abfragen je Sendeversuch
