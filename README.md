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
pio run -e womolin -t upload    # erstes Flashen per USB
pio device monitor              # Logausgabe, 115200 Baud
```

WLAN, IP und Passwörter werden nicht einkompiliert, sondern auf der Setup-Seite der Bridge
eingestellt (siehe [Einrichtung](#einrichtung)). Alle weiteren Updates gehen per WLAN:

```sh
export ALDE_OTA_PASSWORD='…'    # OTA-Passwort aus der Setup-Seite
pio run -e womolin_ota -t upload
```

`ALDE_OTA_PASSWORD` dient beim Bauen außerdem als Start-OTA-Passwort, solange die Bridge noch
keine Einstellungen gespeichert hat. PlatformIO baut bei einer geänderten Variable nicht neu; dann
vorher `rm -rf .pio/build` ausführen.

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

| `/setup` | GET | Einstellungen |
| `/api/config` | GET / POST | Einstellungen lesen (ohne Passwörter) / speichern und neu starten |
| `/api/scan` | GET | WLAN-Suche, so lange abfragen bis `"scanning":false` |

Mit einem Web-Passwort (Setup-Seite) sind alle Seiten geschützt, Benutzer `admin`. Ohne ist die
Bridge für jeden im selben WLAN bedienbar, auch die Einstellungen.

### Einrichtung

Die Einstellungen liegen im Flash der Bridge (NVS) und überstehen Firmware-Updates.

| Einstellung | Bedeutung |
|---|---|
| Betriebsart | **WLAN-Client**: Die Bridge verbindet sich mit dem Router. **Eigener Access Point**: Die Bridge öffnet dauerhaft ein eigenes WLAN, IP `192.168.4.1`. Der Cerbo verbindet sich dann mit diesem WLAN und bekommt `host = 192.168.4.1` in `config.ini`. |
| WLAN | Netzwerk (mit Suche) und Passwort |
| IP-Adresse | DHCP oder fest (IP, Gateway, Subnetzmaske, DNS). Eine feste Adresse muss außerhalb des DHCP-Bereichs liegen, bei der Fritzbox standardmäßig `.20`–`.200`. |
| Access Point | Name (Standard `Alde-Bridge`) und Passwort (Standard `alde-setup`, solange kein eigenes gesetzt ist) |
| OTA-Passwort | Pflicht. Ohne Passwort wären keine Updates per WLAN mehr möglich. |
| Web-Passwort | optional, Benutzer `admin` |

**Erster Start:** Ohne gespeicherte Einstellungen öffnet die Bridge den Access Point `Alde-Bridge`
(Passwort `alde-setup`). Nach dem Verbinden öffnen die meisten Handys die Setup-Seite von selbst,
sonst `http://192.168.4.1` aufrufen.

**Notfall-Zugang:** Ist das eingestellte WLAN 60 s nicht erreichbar (falsches Passwort, Router aus),
öffnet die Bridge zusätzlich ihren Access Point und versucht alle 30 s erneut, ins WLAN zu kommen.
Sobald das klappt und niemand mehr am Access Point hängt, schließt sie ihn wieder. Die Netz-LED
blinkt, solange der Access Point offen ist.

**Update von einer Version mit `secrets.h`:** Beim ersten Start übernimmt die Bridge die
WLAN-Zugangsdaten, die der ESP32 von der Vorversion gespeichert hat. Das OTA-Passwort kommt aus
`ALDE_OTA_PASSWORD` beim Bauen. Danach kann `include/secrets.h` gelöscht werden.

**Rettung über USB:** Ist die Bridge nicht mehr erreichbar, nimmt sie über die USB-Schnittstelle
(115200 Baud) diese Befehle an. Über TCP sind sie gesperrt.

| Befehl | Wirkung |
|---|---|
| `NET` | Betriebsart, WLAN, IP-Adressen anzeigen |
| `NET STA` | zurück auf WLAN-Client (gespeichertes WLAN), Neustart |
| `NET AP` | auf dauerhaften Access Point (braucht ein gespeichertes AP-Passwort), Neustart |

Nach einer Änderung der Adresse `host` in `config.ini` auf dem Cerbo anpassen.

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

1. Controller flashen und über den Access Point `Alde-Bridge` einrichten. Danach ist die Bridge
   unter `http://alde-bridge.local` erreichbar.
2. Mit der Alde verbinden. Nach `Panel antwortet` sollten `STATE link=1 …`-Zeilen erscheinen.
   Die Werte mit dem Display der Alde vergleichen.
3. Einen einzelnen Befehl testen, z.B. über die Weboberfläche den Sollwert ändern, und am Panel prüfen.
4. Danach den Cerbo-Dienst installieren.

Erscheint dauerhaft `STATE link=0`, prüfe Verdrahtung, Masse und 12 V. Hilft das nicht, mit
einem Logikanalysator am LIN-Pin Break und Antwort prüfen.

### Testbetrieb ohne Heizung

```sh
cd firmware
pio run -e womolin_sim -t upload        # per USB
pio run -e womolin_sim_ota -t upload    # per WLAN
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
