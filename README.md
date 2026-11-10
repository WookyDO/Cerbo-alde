# Cerbo-alde

Steuerung einer **Alde Compact 3030 Plus** oder einer **Truma Combi mit CP Plus** über den
**Cerbo GX** (Venus OS) oder eine eigene Weboberfläche, mit dem **WomoLIN LIN-Controller**
(ESP32 + TJA1021T) als Bridge am LIN-Bus der Heizung.

```
Alde 3030 Plus ── gelber RJ12 ──┐
  (Bridge = LIN-Master)         ├─ WomoLIN LIN-Controller ── WLAN/TCP ── Cerbo GX
Truma CP Plus  ── RJ12 (TIN) ───┘    Firmware: firmware/                 Dienst: cerbo/dbus-lin-heater/
  (Bridge = iNet Box, LIN-Slave)     Weboberfläche: http://lin-bridge.local
```

Der Heizungstyp wird auf der Setup-Seite der Bridge gewählt. Der Cerbo-Dienst übernimmt ihn
automatisch.

> **Status:** Protokolle und Logik sind mit Unit-Tests geprüft. Beide Typen laufen im Testbetrieb
> auf dem Controller und dem Cerbo. Der Test an einer echten Alde und an einem echten CP Plus steht
> noch aus. Erst nur lesen (siehe [Inbetriebnahme](#inbetriebnahme)), dann steuern.

## Auf dem Cerbo

**Alde Compact 3030 Plus**

| D-Bus-Dienst | Inhalt |
|---|---|
| `com.victronenergy.switch.alde_3030` | Sollwert (Temperaturregler), Gas ein/aus, Elektro 1/2/3 kW, Warmwasser Aus/An/Boost; Statuswerte unter `/Alde/*` (Pumpe, Gasventil, 230 V, Fehler) |
| `com.victronenergy.temperature.alde_innen` | Innentemperatur |
| `com.victronenergy.temperature.alde_aussen` | Außentemperatur |

**Truma Combi mit CP Plus**

| D-Bus-Dienst | Inhalt |
|---|---|
| `com.victronenergy.switch.truma_combi` | Raumtemperatur (Temperaturregler mit an/aus), Heizstufe Aus/Eco/Boost, Warmwasser Aus/Eco/Heiß/Boost, Energie Gas/Mix/Elektro, Elektro 900/1800 W; Statuswerte unter `/Truma/*` (Betriebsstatus, Fehlercode) |
| `com.victronenergy.temperature.truma_innen` | Innentemperatur |
| `com.victronenergy.temperature.truma_wasser` | Wassertemperatur |

In der GUI v2 erscheinen die Schalter in der Schalter-Ansicht (Gruppe „Alde Heizung“ bzw.
„Truma Heizung“), die Temperaturen unter Temperatursensoren und im VRM.

## Hardware

**ESP32_LIN-Platine** (ESP32-C3-WROOM-02-N4, 2× TJA1021T/20, KiCad-Projekt in [`hardware/`](hardware), Fertigungsdaten in `hardware/fertigung`):

| | Truma (Bridge = Slave) | Alde (Bridge = Master) |
|---|---|---|
| Kanal | LIN1, Klemme J2 / RJ12 J6 | LIN2, Klemme J5 / RJ12 J7, **Jumper JP1 stecken** |
| GPIO RX / TX / SLP | 5 / 6 / 7 | 4 / 10 / 20 |

- Nur der Transceiver der gewählten Heizung wird geweckt (SLP high), der andere schläft.
- Rote LED (GPIO8, aktiv low): an = Heizung verbunden und Netz da, blinkt = Setup- oder
  Notfall-Access-Point offen, aus = keine Verbindung zur Heizung. Grün = Versorgung.
- Bordspannung über VSENSE (GPIO3, Teiler 100k/15k an VIN_F): in der Weboberfläche,
  `/api/state` (`"vin"`), per Befehl `VIN` und in `NET`. Gemessen wird hinter der
  Verpolschutz-Diode; der Wert enthält dafür pauschal 0,24 V (`VSENSE_DIODE_DROP` in `config.h`,
  gemessen bei 12 V). Nur an USB ist die Anzeige nicht aussagekräftig, weil der Strom dort über
  D3 statt über D1 kommt.
- RJ12-Buchsen in Truma-Belegung: Pin 1 = 12 V (VIN_RAW), Pin 2/5 = GND, Pin 3 = LIN.
  Der gelbe Alde-Stecker hat LIN auf Pin 4 und passt deshalb nicht direkt an J7; die Alde
  über die Klemme J5 anschließen.
- Flashen und Log über USB-C (USB-Serial/JTAG des C3, keine Treiber nötig).

**WomoLIN LIN-Controller:**

- WomoLIN LIN-Controller: ESP32 (16 MB), TJA1021T, LIN an UART1 (**RX GPIO14, TX GPIO15**),
  LEDs GPIO2 (LIN) und GPIO17 (Netz). Pinbelegung aus `inetbox2mqtt` (Profil `WOMOLIN`).
- **Alde:** Anschluss am **gelben** RJ12: **Pin 4 = LIN**, **Pin 2 = GND**. Den roten (internen) Bus
  nicht verwenden.
- **Truma:** Anschluss am RJ12 des CP Plus bzw. TIN-Bus: **Pin 3 = LIN**, **Pin 5 = GND**.
  Voraussetzungen: CP Plus mit Firmware **ab C4.00.00** (ältere nur lesen), **keine echte iNet Box**
  am Bus (war je eine angeschlossen, vorher ohne Bridge einmal neu initialisieren). Getestet ist das
  Protokoll in den Quellen mit der Combi 4.
- 12 V für den Controller aus dem Bordnetz. Die Masse muss mit der Heizung verbunden sein.

## Firmware (`firmware/`)

Benötigt [PlatformIO](https://platformio.org/).

```sh
cd firmware
pio run -e esp32_lin -t upload  # ESP32_LIN: erstes Flashen per USB
pio run -e womolin -t upload    # WomoLIN: erstes Flashen per USB
pio device monitor              # Logausgabe, 115200 Baud
```

Für OTA und den Testbetrieb gibt es zu jeder Platine die Varianten `_ota`, `_sim` und `_sim_ota`
(z.B. `esp32_lin_ota`).

WLAN, IP, Passwörter und Heizungstyp werden nicht einkompiliert, sondern auf der Setup-Seite der
Bridge eingestellt (siehe [Einrichtung](#einrichtung)). Alle weiteren Updates gehen per WLAN:

```sh
export LIN_OTA_PASSWORD='…'    # OTA-Passwort aus der Setup-Seite
pio run -e womolin_ota -t upload
```

Die Originalsoftware lässt sich jederzeit wiederherstellen, über
`bin/flash_womolin_inetbox2mqtt_*.bin.zip` aus [inetbox2mqtt](https://github.com/mc0110/inetbox2mqtt).

Eine Firmware für beide Heizungen: Flash 885 KB von 6,4 MB je App-Partition (13,5 %),
RAM 50 KB von 320 KB (15 %).

### Ablauf auf dem Bus: Alde (Bridge = LIN-Master, 19200 Baud)

- Vor dem ersten Senden hört die Bridge 1,5 s auf den Bus. Die Alde-Bedienteile senden nie von
  sich aus; kommen trotzdem Daten, hängt ein anderer Master am Bus (z.B. ein Truma CP Plus). Dann
  sendet die Bridge nichts, meldet `ERR Fremder LIN-Master am Bus` und prüft alle 10 s erneut.
- Beim Start und nach Verbindungsverlust (alle 5 s) wird der Anmelde-Frame `0x3C` gesendet:
  `10 06 B2 00 DE 41 03 00`.
- Alle 500 ms wird der Info-Frame `0x1B` (PID `0x5B`) abgefragt. Das eigene Echo wird verworfen,
  die Antwort wird anhand der Checksumme erkannt.
- Ein Steuer-Frame `0x1A` geht **nur bei einem Befehl** raus, als **Read-Modify-Write** auf Basis des
  zuletzt gelesenen Zustands. Statusbits und Zone 2 bleiben unverändert. Danach wird bis zu
  5-mal abgefragt, ob das Panel den Befehl übernommen hat. Es gibt maximal 3 Sendeversuche.
- Break: `0x00` bei 9600 Baud, das sind etwa 0,94 ms bzw. 18 Bitzeiten bei 19200 Baud.

### Ablauf auf dem Bus: Truma (Bridge = iNet Box, LIN-Slave, 9600 Baud)

- Das CP Plus ist Master. Die Bridge antwortet in einer eigenen FreeRTOS-Task, damit Webserver und
  WLAN die Antwortzeit nicht verzögern.
- Anmeldung (nach „PR SET“ am CP Plus): Lesen der Kennung `0xB2` (Supplier `0x4617`, Function
  `0x1F00`), Zuweisen der Knotenadresse `0xB0` (NAD `0x03`), danach regelmäßig `0xB9`.
- Das CP Plus schickt seinen Status als Puffer `0x14/0x33` über den Transport-Layer (`0x3C`,
  Dienst `0xBB`, 7 Frames). Temperaturen sind Kelvin × 10.
- Auf PID `0x18` meldet die Bridge `FF`, wenn ein Befehl bereitliegt, sonst `FE`. Das CP Plus holt
  ihn dann mit `0xBA` ab (Puffer `0x0C/0x32` auf `0x3D`). Unveränderte Felder werden aus dem
  letzten Status übernommen.
- Das CP Plus fragt nicht ständig; Pausen von 15–25 s sind normal. Bis ein Befehl im Status
  bestätigt ist, können ~30 s vergehen. Ohne Bestätigung nach 60 s wird er erneut eingereiht,
  insgesamt 3 Versuche.
- Regeln des CP Plus, die die Bridge selbst ergänzt: Raumtemperatur und Heizstufe gehören
  zusammen (Sollwert ab 5 °C schaltet Eco ein, Heizstufe „aus“ setzt den Sollwert auf 0), ebenso
  Energie und Leistung (Gas = 0 W, Mix/Elektro = 900 oder 1800 W).

### Zeilenprotokoll (TCP-Port 7020 und USB-Seriell)

Beim Verbinden meldet die Bridge `HELLO lin-bridge 1.1 heater=alde` bzw. `heater=truma`.
Zeilen mit `#` sind Logausgaben.

| Richtung | Zeile | Bedeutung |
|---|---|---|
| → | `GET` / `PING` | Zustand senden / `PONG` |
| → | `SET …` | siehe unten, beliebige Teilmenge |
| ← | `STATE link=1 …` | bei Änderung, sonst alle 5 s |
| ← | `STATE link=0` | Heizung antwortet nicht |
| ← | `OK` / `ERR <Text>` | Befehl angenommen / abgelehnt oder nicht übernommen |

| Heizung | `SET` | `STATE` |
|---|---|---|
| Alde | `sp` 5–30 °C (0,5er Schritte), `gas` 0/1, `el` 0–3 kW, `ww` 0=aus 1=an 2=Boost | `STATE link=1 z1=21.0 z2=- out=8.0 sp=20.0 gas=1 valve=1 el=2 ww=1 on=1 busy=0 err=0 ac=1 pump=1 raw=DE9E29` |
| Truma | `sp` 0 oder 5–30 °C, `mode` off/eco/boost, `ww` 0/40/60/200 (aus/Eco/heiß/Boost), `mix` gas/mix/el, `el` 0/900/1800 W | `STATE link=1 room=19.5 sp=21.0 mode=eco water=48.0 ww=60 mix=gas el=0 status=5 err=0` |

Typspezifische Befehle: `DIAG` (Alde: Anmelde-Frame erneut senden), `LINLOG 1`/`LINLOG 0`
(Truma: alle Frames am Bus als Logzeilen ausgeben).

Zum Testen: `nc <ip> 7020` oder den seriellen Monitor verwenden. Die Bridge hält nur eine
TCP-Verbindung; eine neue verdrängt die bestehende (z.B. die des Cerbo).

### Weboberfläche (Betrieb ohne Cerbo)

Unter `http://lin-bridge.local` (oder der IP der Bridge) gibt es eine Seite mit dem gleichen
Umfang wie auf dem Cerbo, passend zum Heizungstyp. Dazu kommen die WLAN-Signalstärke und ein Link
zu den Einstellungen. Befehle laufen über denselben Weg wie die vom Cerbo; beide lassen sich
gleichzeitig verwenden.

| Pfad | Methode | Inhalt |
|---|---|---|
| `/` | GET | Weboberfläche |
| `/api/state` | GET | `{"type":"alde"\|"truma","state":{…}\|null,"pending":bool,"error":"…","rssi":-67,"ap":bool}` |
| `/api/set` | POST | Formularfelder wie bei `SET`; Antwort `{"ok":true}` oder `{"ok":false,"error":"…"}` |
| `/setup` | GET | Einstellungen |
| `/api/config` | GET / POST | Einstellungen lesen (ohne Passwörter) / speichern |
| `/api/scan` | GET | WLAN-Suche, so lange abfragen bis `"scanning":false` |

Mit einem Web-Passwort (Setup-Seite) sind alle Seiten geschützt, Benutzer `admin`. Ohne ist die
Bridge für jeden im selben WLAN bedienbar, auch die Einstellungen.

### Einrichtung

Die Einstellungen liegen im Flash der Bridge (NVS) und überstehen Firmware-Updates.

| Einstellung | Bedeutung |
|---|---|
| Heizung | Alde Compact 3030 Plus oder Truma Combi mit CP Plus. Ein Wechsel startet die Bridge neu. |
| Betriebsart | **WLAN-Client**: Die Bridge verbindet sich mit dem Router. **Eigener Access Point**: Die Bridge öffnet dauerhaft ein eigenes WLAN, IP `192.168.4.1`. Der Cerbo verbindet sich dann mit diesem WLAN und bekommt `host = 192.168.4.1` in `config.ini`. |
| WLAN | Netzwerk (mit Suche) und Passwort |
| IP-Adresse | DHCP oder fest (IP, Gateway, Subnetzmaske, DNS). Eine feste Adresse muss außerhalb des DHCP-Bereichs liegen, bei der Fritzbox standardmäßig `.20`–`.200`. |
| Access Point | Name (Standard `LIN-Bridge`) und Passwort (Standard `lin-setup`, solange kein eigenes gesetzt ist) |
| OTA-Passwort | Pflicht. Updates per WLAN sind erst nach der Einrichtung möglich, nie ohne Passwort. |
| Web-Passwort | optional, Benutzer `admin` |

Speichern ohne Änderung tut nichts. Neue OTA- oder Web-Passwörter gelten sofort. Bei allem anderen
startet die Bridge neu; die Seite wartet, bis sie wieder antwortet.

**Erster Start:** Ohne gespeicherte Einstellungen öffnet die Bridge den Access Point `LIN-Bridge`
(Passwort `lin-setup`). Nach dem Verbinden öffnen die meisten Handys die Setup-Seite von selbst,
sonst `http://192.168.4.1` aufrufen.

**Notfall-Zugang:** Ist das eingestellte WLAN 60 s nicht erreichbar (falsches Passwort, Router aus),
öffnet die Bridge zusätzlich ihren Access Point und versucht alle 30 s erneut, ins WLAN zu kommen.
Sobald das klappt und niemand mehr am Access Point hängt, schließt sie ihn wieder. Die Netz-LED
blinkt, solange der Access Point offen ist.

**Rettung über USB:** Ist die Bridge nicht mehr erreichbar, nimmt sie über die USB-Schnittstelle
(115200 Baud) diese Befehle an. Über TCP sind sie gesperrt.

| Befehl | Wirkung |
|---|---|
| `NET` | Betriebsart, WLAN, IP-Adressen und Heizungstyp anzeigen |
| `NET STA` | zurück auf WLAN-Client (gespeichertes WLAN), Neustart |
| `NET AP` | auf dauerhaften Access Point (braucht ein gespeichertes AP-Passwort), Neustart |

Nach einer Änderung der Adresse `host` in `config.ini` auf dem Cerbo anpassen.

## Cerbo-Dienst (`cerbo/dbus-lin-heater/`)

Voraussetzung ist SSH-Zugang als root: Einstellungen → Allgemein → Zugriffsebene „Superuser“,
Root-Passwort setzen, SSH im LAN aktivieren.

```sh
scp -r cerbo/dbus-lin-heater root@<cerbo-ip>:/data/
ssh root@<cerbo-ip> /data/dbus-lin-heater/install.sh
ssh root@<cerbo-ip> vi /data/dbus-lin-heater/config.ini   # host = IP der Bridge
ssh root@<cerbo-ip> svc -t /service/dbus-lin-heater       # neu starten
```

- Heizungstyp: Mit `heater = auto` (Standard) übernimmt der Dienst den Typ aus dem `HELLO` der
  Bridge. Meldet sie einen anderen als den laufenden, merkt er ihn sich und startet mit den
  passenden D-Bus-Diensten neu. Mit `heater = alde` oder `truma` bleibt er fest; ein abweichender
  Typ wird dann nur gemeldet und nicht ausgewertet.
- Truma: Nach einer Eingabe in der GUI hält der Dienst den neuen Wert 40 s, bis das CP Plus ihn
  bestätigt hat (Alde: 3 s).
- Logs: `tail -F /var/log/dbus-lin-heater/current | tai64nlocal`
- Deinstallieren: `/data/dbus-lin-heater/uninstall.sh`
- Der Dienst übersteht Firmware-Updates, weil er unter `/data` liegt und über `/data/rc.local` gestartet wird.

## Inbetriebnahme

**Alde**

1. Controller flashen, über den Access Point `LIN-Bridge` einrichten, Heizung „Alde“ wählen.
2. Mit der Alde verbinden. Nach `Panel antwortet` sollten `STATE link=1 …`-Zeilen erscheinen.
   Die Werte mit dem Display der Alde vergleichen.
3. Einen einzelnen Befehl testen, z.B. über die Weboberfläche den Sollwert ändern, und am Panel prüfen.
4. Danach den Cerbo-Dienst installieren.

Erscheint dauerhaft `STATE link=0`, prüfe Verdrahtung, Masse und 12 V. Hilft das nicht, mit
einem Logikanalysator am LIN-Pin Break und Antwort prüfen.

**Truma**

1. Controller flashen, einrichten, Heizung „Truma“ wählen.
2. An das CP Plus anschließen und mit `LINLOG 1` (USB oder `nc <ip> 7020`) mithören. Es sollten
   Header `18`, `3C` und `3D` erscheinen. Kommt nach 60 s nichts, meldet die Weboberfläche
   „Kein CP Plus gefunden“.
3. Am CP Plus im Einstellungsmenü **„PR SET“** ausführen. Das Display zeigt „INIT…“, danach steht im
   Menü ein dritter Eintrag `inetbox`. Die Bridge meldet `CP Plus verbunden`.
4. Werte mit dem CP Plus vergleichen, dann einzelne Befehle testen. Bis zur Bestätigung können
   ~30 s vergehen.
5. Danach den Cerbo-Dienst installieren.

### Testbetrieb ohne Heizung

```sh
cd firmware
pio run -e womolin_sim -t upload        # per USB
pio run -e womolin_sim_ota -t upload    # per WLAN
```

- **Alde:** Der Controller beantwortet seine LIN-Abfragen selbst wie ein Alde-Panel. Die Antworten
  laufen über den echten Transceiver und kommen als Echo zurück; Steuer-Frames liest das simulierte
  Panel ebenfalls aus dem Echo. Damit lassen sich LIN-Hardware, WLAN/TCP und der Cerbo-Dienst
  testen. Der Controller braucht dafür 12 V, sonst bleibt es bei `link=0`. Das Panel startet mit
  18 °C innen, 8 °C außen und Sollwert 20 °C und heizt bei Gas oder Elektro mit 0,05 °C/s.
- **Truma:** Ein simuliertes CP Plus spielt den kompletten Transport-Layer in Software gegen die
  iNet Box durch, ohne Bus. Damit lassen sich Weboberfläche, TCP und Cerbo-Dienst testen, nicht aber
  der LIN-Slave-Treiber. Start: 15 °C innen, Wasser 20 °C, Heizung aus.

Die Testvariante **nicht an der echten Heizung** verwenden. Vor dem Anschluss wieder
`pio run -e womolin -t upload` (oder per WLAN `womolin_ota`) aufspielen.

## Tests

```sh
cd cerbo/dbus-lin-heater && python3 -m unittest discover -s tests   # Cerbo-Dienst
cd firmware && pio test -e native                                    # Protokolle (C++)
```

## Quellen

- [daviddootson/alde_3030_plus](https://github.com/daviddootson/alde_3030_plus): Protokoll des
  CI-Busses der 3030 Plus (Mitschnitte, Anmelde-Frame, Bitbelegung)
- [danielfett/inetbox.py](https://github.com/danielfett/inetbox.py): Protokoll der Truma iNet Box
  (Transport-Layer, Puffer, Prüfsumme, Referenzwerte für die Tests). Der Code hier ist eine eigene
  Umsetzung nach dieser Beschreibung; inetbox.py steht unter GPL-3.0.
- [mc0110/inetbox2mqtt](https://github.com/mc0110/inetbox2mqtt): Pinbelegung des WomoLIN-Controllers,
  Voraussetzungen und Einrichtung am CP Plus
- [WomoLIN](https://github.com/muccc/WomoLIN): Grundlagen zum LIN-Bus im Wohnmobil
- [victronenergy/gui-v2](https://github.com/victronenergy/gui-v2): Schnittstelle `SwitchableOutput`
