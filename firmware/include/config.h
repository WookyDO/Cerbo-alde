// Hardware- und Netzwerkeinstellungen der LIN-Heizungs-Bridge.
// WLAN, IP und Passwörter werden auf der Setup-Seite eingestellt (settings.h).
#pragma once

#include <cstdint>

// WomoLIN LIN-Controller (Pinbelegung wie inetbox2mqtt, Profil "WOMOLIN")
constexpr int PIN_LIN_RX = 14;
constexpr int PIN_LIN_TX = 15;
constexpr int PIN_LED_LIN = 2;   // aktiv low
constexpr int PIN_LED_NET = 17;  // aktiv low

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
