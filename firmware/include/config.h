// Hardware- und Netzwerkeinstellungen der Alde-LIN-Bridge.
#pragma once

// Zugangsdaten liegen in secrets.h (nicht im Repo, Vorlage: secrets.example.h).
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h fehlt – secrets.example.h kopieren und anpassen"
#endif

// WomoLIN LIN-Controller (Pinbelegung wie inetbox2mqtt, Profil "WOMOLIN")
constexpr int PIN_LIN_RX = 14;
constexpr int PIN_LIN_TX = 15;
constexpr int PIN_LED_LIN = 2;   // aktiv low
constexpr int PIN_LED_NET = 17;  // aktiv low

constexpr const char *HOSTNAME = "alde-bridge";
constexpr uint16_t TCP_PORT = 7020;
constexpr uint16_t HTTP_PORT = 80;

constexpr uint32_t POLL_INTERVAL_MS = 500;      // normale Statusabfrage
constexpr uint32_t CONFIRM_INTERVAL_MS = 300;   // Abfrage nach einem Befehl
constexpr uint32_t STATE_HEARTBEAT_MS = 5000;   // STATE auch ohne Änderung
constexpr uint8_t LINK_LOST_AFTER = 6;          // Fehlversuche bis link=0
constexpr uint8_t COMMAND_RETRIES = 3;          // Sendeversuche pro Befehl
constexpr uint8_t COMMAND_CONFIRM_POLLS = 5;    // Abfragen je Sendeversuch
