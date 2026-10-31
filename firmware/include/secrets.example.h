// Nach include/secrets.h kopieren und anpassen. secrets.h wird nicht eingecheckt.
#pragma once

#define WIFI_SSID "mein-wlan"
#define WIFI_PASSWORD "geheim"

// Passwort für Firmware-Updates per WLAN (env:womolin_ota)
#define OTA_PASSWORD "ota-geheim"

// Optional: feste IP statt DHCP. Adresse außerhalb des DHCP-Bereichs des
// Routers wählen. SUBNET und DNS dürfen fehlen (255.255.255.0 bzw. Gateway).
// #define STATIC_IP "192.168.178.140"
// #define STATIC_GATEWAY "192.168.178.1"
// #define STATIC_SUBNET "255.255.255.0"
// #define STATIC_DNS "192.168.178.1"

// Optional: Weboberfläche (http://alde-bridge.local) mit Passwort schützen,
// Benutzername "admin". Ohne Eintrag ist sie im LAN frei zugänglich.
// #define WEB_PASSWORD "web-geheim"
