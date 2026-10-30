#!/usr/bin/env python3
"""Venus-OS-Dienst für die Alde Compact 3030 Plus.

Verbindet sich per TCP mit der Alde-LIN-Bridge (WomoLIN LIN-Controller) und
stellt auf dem D-Bus bereit:

  com.victronenergy.switch.alde_3030       Bedienung (GUI v2: Schalter-Ansicht)
    /SwitchableOutput/0  Temperatur-Sollwert (Typ 3), Messwert = Innentemperatur
    /SwitchableOutput/1  Gas ein/aus (Typ 1)
    /SwitchableOutput/2  Elektro 1/2/3 kW (Stufenschalter, Typ 4)
    /SwitchableOutput/3  Warmwasser Aus/An/Boost (Dropdown, Typ 6)
  com.victronenergy.temperature.alde_innen Innentemperatur
  com.victronenergy.temperature.alde_aussen Außentemperatur
"""

import configparser
import logging
import os
import socket
import sys
import threading
import time

sys.path.insert(1, '/opt/victronenergy/dbus-systemcalc-py/ext/velib_python')

import dbus  # noqa: E402
from dbus.mainloop.glib import DBusGMainLoop  # noqa: E402
from gi.repository import GLib  # noqa: E402
from vedbus import VeDbusService  # noqa: E402

import alde_model as m  # noqa: E402

VERSION = '1.0'
HERE = os.path.dirname(os.path.abspath(__file__))
log = logging.getLogger('dbus-alde')

# gui-v2 enums.h: SwitchableOutput_Type / _Status, Temperature_DeviceType
TYPE_TOGGLE, TYPE_SETPOINT, TYPE_STEPPED, TYPE_DROPDOWN = 1, 3, 4, 6
STATUS_OFF, STATUS_ON = 0x00, 0x09
TEMP_ROOM, TEMP_OUTDOOR = 3, 4

OUT_SETPOINT, OUT_GAS, OUT_ELECTRIC, OUT_WATER = range(4)

# Nach einer Eingabe in der GUI werden ältere STATE-Werte für dieses Feld so
# lange ignoriert, bis das Panel den Befehl bestätigt haben sollte.
WRITE_HOLD_S = 3.0


class BridgeLink:
    """TCP-Verbindung zur Bridge in einem eigenen Thread; Zeilen und
    Verbindungswechsel werden per GLib.idle_add im Hauptthread gemeldet."""

    def __init__(self, host, port, on_line, on_connection):
        self.host, self.port = host, port
        self.on_line, self.on_connection = on_line, on_connection
        self.sock = None
        self.lock = threading.Lock()

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()

    def send(self, line):
        with self.lock:
            if self.sock is None:
                return False
            try:
                self.sock.sendall((line + '\n').encode())
                return True
            except OSError as e:
                log.warning('Senden fehlgeschlagen: %s', e)
                return False

    def _run(self):
        while True:
            try:
                sock = socket.create_connection((self.host, self.port), timeout=10)
                # Die Bridge sendet spätestens alle 5 s eine STATE-Zeile.
                sock.settimeout(15)
                with self.lock:
                    self.sock = sock
                log.info('Verbunden mit %s:%d', self.host, self.port)
                GLib.idle_add(self.on_connection, True)
                self._read(sock)
            except OSError as e:
                log.warning('Bridge %s:%d: %s', self.host, self.port, e)
            with self.lock:
                if self.sock is not None:
                    self.sock.close()
                    self.sock = None
                    GLib.idle_add(self.on_connection, False)
            time.sleep(5)

    def _read(self, sock):
        buf = b''
        while True:
            chunk = sock.recv(1024)
            if not chunk:
                raise OSError('Verbindung geschlossen')
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                GLib.idle_add(self.on_line, line.decode(errors='replace'))


def new_bus():
    if 'DBUS_SESSION_BUS_ADDRESS' in os.environ:
        return dbus.SessionBus(private=True)
    return dbus.SystemBus(private=True)


class AldeService:
    def __init__(self, cfg):
        host = cfg.get('bridge', 'host')
        port = cfg.getint('bridge', 'port')
        names = cfg['names']
        self.link = BridgeLink(host, port, self.on_line, self.on_connection)
        self.state = None
        self.connected = False
        self.electric_level = 1
        self.hold = {}
        connection = 'TCP %s:%d' % (host, port)

        self.sw = VeDbusService('com.victronenergy.switch.alde_3030', new_bus(), register=False)
        self._mandatory(self.sw, connection, cfg.getint('dbus', 'switch_instance'),
                        'Alde Compact 3030 Plus')
        group = names.get('group')
        self._output(OUT_SETPOINT, names.get('setpoint'), TYPE_SETPOINT, group,
                     dimming=20.0, measurement=True,
                     settings={'DimmingMin': m.SETPOINT_MIN, 'DimmingMax': m.SETPOINT_MAX,
                               'StepSize': 0.5})
        self._output(OUT_GAS, names.get('gas'), TYPE_TOGGLE, group, state_writeable=True)
        self._output(OUT_ELECTRIC, names.get('electric'), TYPE_STEPPED, group,
                     state_writeable=True, dimming=1, settings={'DimmingMax': m.ELECTRIC_MAX})
        self._output(OUT_WATER, names.get('water'), TYPE_DROPDOWN, group,
                     dimming=0, settings={'Labels': m.WATER_LABELS})
        for path in ('PanelOn', 'Pump', 'GasValve', 'AcInput', 'Error', 'Busy',
                     'OutdoorTemperature', 'Raw'):
            self.sw.add_path('/Alde/' + path, None)
        self.sw.register()

        self.indoor = self._temperature('alde_innen', cfg.getint('dbus', 'indoor_instance'),
                                        names.get('indoor'), TEMP_ROOM, connection)
        self.outdoor = self._temperature('alde_aussen', cfg.getint('dbus', 'outdoor_instance'),
                                         names.get('outdoor'), TEMP_OUTDOOR, connection)

    # ── D-Bus-Aufbau ────────────────────────────────────────────────────────

    @staticmethod
    def _mandatory(svc, connection, instance, product, custom_name=''):
        svc.add_mandatory_paths(__file__, VERSION, connection, instance, 0xFFFF, product,
                                firmwareversion=VERSION, hardwareversion='WomoLIN',
                                connected=0)
        svc.add_path('/CustomName', custom_name)

    def _output(self, idx, name, type_, group, state_writeable=False, dimming=None,
                measurement=False, settings=None):
        base = '/SwitchableOutput/%d' % idx
        self.sw.add_path(base + '/Name', name)
        self.sw.add_path(base + '/Status', STATUS_OFF)
        self.sw.add_path(base + '/State', 0, writeable=state_writeable,
                         onchangecallback=self._on_write)
        if dimming is not None:
            self.sw.add_path(base + '/Dimming', dimming, writeable=True,
                             onchangecallback=self._on_write)
        if measurement:
            self.sw.add_path(base + '/Measurement', None)
        self.sw.add_path(base + '/Settings/Type', type_)
        self.sw.add_path(base + '/Settings/ValidTypes', 1 << type_)
        self.sw.add_path(base + '/Settings/CustomName', '')
        self.sw.add_path(base + '/Settings/Group', group)
        self.sw.add_path(base + '/Settings/ShowUIControl', 1)
        for key, value in (settings or {}).items():
            self.sw.add_path(base + '/Settings/' + key, value)

    def _temperature(self, name, instance, custom_name, temp_type, connection):
        svc = VeDbusService('com.victronenergy.temperature.' + name, new_bus(), register=False)
        self._mandatory(svc, connection, instance, 'Alde Compact 3030 Plus', custom_name)
        svc.add_path('/Temperature', None)
        svc.add_path('/TemperatureType', temp_type)
        svc.add_path('/Status', 1)  # 0 = OK, 1 = nicht verbunden
        svc.register()
        return svc

    # ── Bridge → D-Bus ──────────────────────────────────────────────────────

    def on_connection(self, up):
        if not up:
            self.on_line('STATE link=0')
        else:
            self.link.send('GET')
        return False

    def on_line(self, line):
        try:
            kind, data = m.parse_line(line)
        except (ValueError, KeyError) as e:
            log.warning('Ungültige Zeile %r: %s', line, e)
            return False
        if kind == 'STATE':
            self._apply(data)
        elif kind == 'ERR':
            log.warning('Bridge meldet Fehler: %s', data)
        elif kind == 'HELLO':
            log.info('Bridge: %s', data)
        return False

    def _set(self, path, value):
        if time.monotonic() < self.hold.get(path, 0):
            return
        self.sw[path] = value

    def _apply(self, s):
        self.state = s
        self.connected = s is not None
        for svc in (self.sw, self.indoor, self.outdoor):
            svc['/Connected'] = 1 if self.connected else 0

        if s is None:
            for svc in (self.indoor, self.outdoor):
                svc['/Temperature'] = None
                svc['/Status'] = 1
            for idx in range(4):
                self.sw['/SwitchableOutput/%d/Status' % idx] = STATUS_OFF
            self.sw['/SwitchableOutput/%d/Measurement' % OUT_SETPOINT] = None
            return

        self.indoor['/Temperature'] = s.zone1
        self.indoor['/Status'] = 0 if s.zone1 is not None else 1
        self.outdoor['/Temperature'] = s.outdoor
        self.outdoor['/Status'] = 0 if s.outdoor is not None else 1

        out = '/SwitchableOutput/%d/'
        self.sw[out % OUT_SETPOINT + 'State'] = 1 if s.heating else 0
        self.sw[out % OUT_SETPOINT + 'Measurement'] = s.zone1
        self._set(out % OUT_SETPOINT + 'Dimming', s.setpoint)

        self._set(out % OUT_GAS + 'State', 1 if s.gas else 0)

        if s.electric > 0:
            self.electric_level = s.electric
        state, level = m.electric_to_switch(s.electric, self.electric_level)
        self._set(out % OUT_ELECTRIC + 'State', state)
        self._set(out % OUT_ELECTRIC + 'Dimming', level)

        self.sw[out % OUT_WATER + 'State'] = 1 if s.water else 0
        self._set(out % OUT_WATER + 'Dimming', s.water)

        for idx, on in ((OUT_SETPOINT, s.heating), (OUT_GAS, s.gas),
                        (OUT_ELECTRIC, s.electric > 0), (OUT_WATER, s.water > 0)):
            self.sw[out % idx + 'Status'] = STATUS_ON if on else STATUS_OFF

        self.sw['/Alde/PanelOn'] = int(s.panel_on)
        self.sw['/Alde/Pump'] = int(s.pump)
        self.sw['/Alde/GasValve'] = int(s.valve)
        self.sw['/Alde/AcInput'] = int(s.ac)
        self.sw['/Alde/Error'] = int(s.error)
        self.sw['/Alde/Busy'] = int(s.busy)
        self.sw['/Alde/OutdoorTemperature'] = s.outdoor
        self.sw['/Alde/Raw'] = s.raw

    # ── D-Bus → Bridge ──────────────────────────────────────────────────────

    def _on_write(self, path, value):
        if not self.connected:
            log.warning('Schreiben auf %s abgelehnt: Panel nicht verbunden', path)
            return False
        idx = int(path.split('/')[2])
        field = path.rsplit('/', 1)[1]
        try:
            if idx == OUT_SETPOINT and field == 'Dimming':
                line = m.format_set(setpoint=value)
            elif idx == OUT_GAS and field == 'State':
                line = m.format_set(gas=bool(value))
            elif idx == OUT_ELECTRIC:
                state = value if field == 'State' else self.sw['/SwitchableOutput/2/State']
                level = value if field == 'Dimming' else self.sw['/SwitchableOutput/2/Dimming']
                if field == 'Dimming':
                    self.electric_level = m.switch_to_electric(1, level)
                    if not state:
                        return True  # nur Stufe vormerken, Elektro bleibt aus
                line = m.format_set(electric=m.switch_to_electric(state, level))
            elif idx == OUT_WATER and field == 'Dimming':
                line = m.format_set(water=int(value))
            else:
                return False
        except (TypeError, ValueError) as e:
            log.warning('Ungültiger Wert %r für %s: %s', value, path, e)
            return False

        log.info('%s = %r -> %s', path, value, line)
        if not self.link.send(line):
            return False
        self.hold[path] = time.monotonic() + WRITE_HOLD_S
        return True


def load_config():
    cfg = configparser.ConfigParser()
    cfg.read_dict({
        'bridge': {'host': '192.168.1.50', 'port': '7020'},
        'dbus': {'switch_instance': '60', 'indoor_instance': '61', 'outdoor_instance': '62'},
        'names': {'group': 'Alde Heizung', 'setpoint': 'Raumtemperatur', 'gas': 'Gas',
                  'electric': 'Elektro (kW)', 'water': 'Warmwasser',
                  'indoor': 'Alde Innen', 'outdoor': 'Alde Aussen'},
    })
    cfg.read(os.path.join(HERE, 'config.ini'), encoding='utf-8')
    return cfg


def main():
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    DBusGMainLoop(set_as_default=True)
    service = AldeService(load_config())
    service.link.start()
    log.info('dbus-alde %s gestartet', VERSION)
    GLib.MainLoop().run()


if __name__ == '__main__':
    main()
