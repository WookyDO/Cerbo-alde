# Cerbo-alde

Steuerung einer **Alde Compact 3030 Plus** über den **Cerbo GX** (Venus OS), mit dem
**WomoLIN LIN-Controller** (ESP32 + TJA1021T) als LIN-Master am gelben CI-Bus.

```
Alde 3030 Plus ── gelber RJ12 ── WomoLIN LIN-Controller ── WLAN/TCP ── Cerbo GX
                  (CI-Bus, LIN)   Firmware: firmware/                  Dienst: cerbo/dbus-alde/
```

Auf dem Cerbo erscheinen:

| D-Bus-Dienst | Inhalt | GUI v2 |
|---|---|---|
| `com.victronenergy.switch.alde_3030` | Sollwert (Temperaturregler), Gas ein/aus, Elektro 1/2/3 kW, Warmwasser Aus/An/Boost | Schalter-Ansicht, Gruppe „Alde Heizung“ |
| `com.victronenergy.temperature.alde_innen` | Innentemperatur | Temperatursensoren, VRM |
| `com.victronenergy.temperature.alde_aussen` | Außentemperatur | Temperatursensoren, VRM |

Zusätzliche Statuswerte (Pumpe, Gasventil, 230 V, Fehler) liegen unter `/Alde/*` im Switch-Dienst.

> **Status:** Protokoll und Logik sind mit Unit-Tests geprüft. Der Test an der echten Heizung
> steht noch aus. Erst nur lesen (siehe [Inbetriebnahme](#inbetriebnahme)), dann steuern.

## Hardware

- WomoLIN LIN-Controller: ESP32 (16 MB), TJA1021T, LIN an UART1 (**RX GPIO14, TX GPIO15**),
  LEDs GPIO2 (LIN) und GPIO17 (Netz). Pinbelegung aus `inetbox2mqtt` (Profil `WOMOLIN`).
- Anschluss am **gelben** RJ12 der Alde: **Pin 4 = LIN**, **Pin 2 = GND**. Den roten (internen) Bus
  nicht verwenden.
- 12 V für den Controller aus dem Bordnetz. Die Masse muss mit der Alde verbunden sein.

## Firmware (`firmware/`)

Benötigt [PlatformIO](https://platformio.org/).

```sh
cd firmware
cp include/secrets.example.h include/secrets.h   # WLAN + OTA-Passwort eintragen
pio run -e womolin -t upload                      # per USB flashen
pio device monitor                                # Logausgabe, 115200 Baud
```

Spätere Updates gehen per WLAN: `ALDE_OTA_PASSWORD=... pio run -e womolin_ota -t upload`.

Die Originalsoftware lässt sich jederzeit wiederherstellen, über
`bin/flash_womolin_inetbox2mqtt_*.bin.zip` aus [inetbox2mqtt](https://github.com/mc0110/inetbox2mqtt).

### Ablauf auf dem Bus

- Beim Start und nach Verbindungsverlust (alle 5 s) wird der Anmelde-Frame `0x3C` gesendet:
  `10 06 B2 00 DE 41 03 00`.
- Alle 500 ms wird der Info-Frame `0x1B` (PID `0x5B`) abgefragt. Das eigene Echo wird verworfen,
  die Antwort wird anhand der Checksumme erkannt.
- Ein Steuer-Frame `0x1A` geht **nur bei einem Befehl** raus, als **Read-Modify-Write** auf Basis des
  zuletzt gelesenen Zustands. Statusbits und Zone 2 bleiben unverändert. Danach wird bis zu
  5-mal abgefragt, ob das Panel den Befehl übernommen hat. Es gibt maximal 3 Sendeversuche.
- Break: `0x00` bei 9600 Baud, das sind etwa 0,94 ms bzw. 18 Bitzeiten bei 19200 Baud.

### Zeilenprotokoll (TCP-Port 7020 und USB-Seriell)

| Richtung | Zeile | Bedeutung |
|---|---|---|
| → | `SET sp=21.5 gas=1 el=2 ww=0` | beliebige Teilmenge. `sp` 5–30 °C, `gas` 0/1, `el` 0–3 kW, `ww` 0=aus 1=an 2=boost |
| → | `GET` / `PING` / `DIAG` | Zustand senden / `PONG` / Anmelde-Frame erneut senden |
| ← | `STATE link=1 z1=21.0 z2=- out=8.0 sp=20.0 gas=1 valve=1 el=2 ww=1 on=1 busy=0 err=0 ac=1 pump=1 raw=DE9E29` | bei Änderung, sonst alle 5 s |
| ← | `STATE link=0` | Panel antwortet nicht |
| ← | `OK` / `ERR <Text>` | Befehl angenommen / abgelehnt oder vom Panel nicht übernommen |

Zum Testen: `nc <ip> 7020` oder den seriellen Monitor verwenden.

### Weboberfläche (Betrieb ohne Cerbo)

Unter `http://alde-bridge.local` (oder der IP der Bridge) gibt es eine Seite mit dem gleichen
Umfang wie auf dem Cerbo: Innen- und Außentemperatur, Sollwert, Gas, Elektro 0–3 kW,
Warmwasser Aus/An/Boost sowie Pumpe, Gasventil, 230 V, Fehler und die WLAN-Signalstärke.
Befehle laufen über denselben Weg wie die vom Cerbo (Read-Modify-Write, Bestätigung durch das
Panel). Beide lassen sich gleichzeitig verwenden.

| Pfad | Methode | Inhalt |
|---|---|---|
| `/` | GET | Weboberfläche |
| `/api/state` | GET | `{"state":{…}\|null,"pending":bool,"error":"…","rssi":-67}` |
| `/api/set` | POST | Formularfelder `sp`, `gas`, `el`, `ww` wie bei `SET`; Antwort `{"ok":true}` oder `{"ok":false,"error":"…"}` |

Mit `WEB_PASSWORD` in `secrets.h` ist die Seite per Passwort geschützt (Benutzer `admin`).
Ohne Eintrag ist sie im LAN frei zugänglich.

### Feste IP-Adresse

Standardmäßig holt sich die Bridge ihre Adresse per DHCP. Für eine feste Adresse in
`include/secrets.h` eintragen (die Adresse muss außerhalb des DHCP-Bereichs des Routers liegen;
bei der Fritzbox ist das standardmäßig `.20`–`.200`):

```c
#define STATIC_IP "192.168.178.240"
#define STATIC_GATEWAY "192.168.178.1"
// optional: STATIC_SUBNET (Standard 255.255.255.0), STATIC_DNS (Standard: Gateway)
```

Ist eine Angabe ungültig, meldet das Log `Feste IP ungueltig, verwende DHCP`. Nach einer Änderung
der Adresse `host` in `config.ini` auf dem Cerbo anpassen.

## Cerbo-Dienst (`cerbo/dbus-alde/`)

Voraussetzung ist SSH-Zugang als root: Einstellungen → Allgemein → Zugriffsebene „Superuser“,
Root-Passwort setzen, SSH im LAN aktivieren.

```sh
scp -r cerbo/dbus-alde root@<cerbo-ip>:/data/
ssh root@<cerbo-ip> /data/dbus-alde/install.sh
ssh root@<cerbo-ip> vi /data/dbus-alde/config.ini   # host = IP des LIN-Controllers
ssh root@<cerbo-ip> svc -t /service/dbus-alde       # neu starten
```

- Logs: `tail -F /var/log/dbus-alde/current | tai64nlocal`
- Deinstallieren: `/data/dbus-alde/uninstall.sh`
- Der Dienst übersteht Firmware-Updates, weil er unter `/data` liegt und über `/data/rc.local` gestartet wird.
- Weise dem Controller im Router eine feste IP-Adresse zu.

## Inbetriebnahme

1. Controller flashen, per USB den Monitor öffnen und prüfen, dass `WLAN verbunden, IP …` erscheint.
2. Mit der Alde verbinden. Nach `Panel antwortet` sollten `STATE link=1 …`-Zeilen erscheinen.
   Die Werte mit dem Display der Alde vergleichen.
3. Einen einzelnen Befehl testen, z.B. `SET sp=21` eintippen, und am Panel prüfen.
4. Danach den Cerbo-Dienst installieren.

Erscheint dauerhaft `STATE link=0`, prüfe Verdrahtung, Masse und 12 V. Hilft das nicht, mit
einem Logikanalysator am LIN-Pin Break und Antwort prüfen.

### Testbetrieb ohne Heizung

```sh
cd firmware
pio run -e womolin_sim -t upload
```

In dieser Variante beantwortet der Controller seine LIN-Abfragen selbst wie ein Alde-Panel. Die
Antworten laufen über den echten Transceiver und kommen als Echo zurück; Steuer-Frames liest das
simulierte Panel ebenfalls aus dem Echo. Damit lassen sich LIN-Hardware, WLAN/TCP und der
Cerbo-Dienst testen, bevor die Alde angeschlossen ist. Der Controller braucht dafür 12 V, sonst
bleibt es bei `link=0`.

Das simulierte Panel startet mit 18 °C innen, 8 °C außen und Sollwert 20 °C. Bei Gas oder Elektro
heizt es mit 0,05 °C/s Richtung Sollwert, sonst kühlt es Richtung Außentemperatur ab.

Die Testvariante **nicht an der echten Heizung** verwenden. Vor dem Anschluss an die Alde wieder
`pio run -e womolin -t upload` (oder per WLAN `womolin_ota`) aufspielen.

## Tests

```sh
cd cerbo/dbus-alde && python3 -m unittest discover -s tests   # Cerbo-Dienst
cd firmware && pio test -e native                              # Protokoll (C++)
```

## Quellen

- [daviddootson/alde_3030_plus](https://github.com/daviddootson/alde_3030_plus): Protokoll des
  CI-Busses der 3030 Plus (Mitschnitte, Anmelde-Frame, Bitbelegung)
- [mc0110/inetbox2mqtt](https://github.com/mc0110/inetbox2mqtt): Pinbelegung des WomoLIN-Controllers
- [WomoLIN](https://github.com/muccc/WomoLIN): Grundlagen zum LIN-Bus im Wohnmobil
- [victronenergy/gui-v2](https://github.com/victronenergy/gui-v2): Schnittstelle `SwitchableOutput`
