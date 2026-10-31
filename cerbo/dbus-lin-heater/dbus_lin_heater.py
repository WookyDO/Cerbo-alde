#!/usr/bin/env python3
"""Venus-OS-Dienst für die LIN-Heizungs-Bridge (Alde Compact 3030 Plus oder
Truma Combi mit CP Plus).

Verbindet sich per TCP mit der Bridge (WomoLIN LIN-Controller) und stellt einen
Switch-Dienst (GUI v2: Schalter-Ansicht) und Temperaturdienste bereit. Welche,
hängt vom Heizungstyp ab (models/). Den Typ meldet die Bridge im HELLO; meldet
sie einen anderen als den laufenden, startet sich der Dienst mit dem passenden
Modell neu (bei heater = auto).
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

from models import MODELS, parse_hello  # noqa: E402
from models.common import STATUS_OFF  # noqa: E402

VERSION = '1.1'
HERE = os.path.dirname(os.path.abspath(__file__))
TYPE_CACHE = os.path.join(HERE, 'heater.type')
log = logging.getLogger('dbus-lin-heater')


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


def model_names(cfg, model):
    """Anzeigenamen: Vorgaben des Modells, überschrieben aus [names.<typ>].
    Für die Alde gilt auch [names] (Konfiguration von dbus-alde)."""
    names = dict(model.NAMES)
    sections = (['names'] if model.TYPE == 'alde' else []) + ['names.' + model.TYPE]
    for section in sections:
        if cfg.has_section(section):
            names.update(cfg[section])
    return names


def save_type(heater):
    try:
        with open(TYPE_CACHE, 'w') as f:
            f.write(heater)
    except OSError as e:
        log.warning('Heizungstyp nicht gespeichert: %s', e)


def restart_process():
    # daemontools startet den Dienst neu, dann mit dem neuen Modell.
    os._exit(0)


class HeaterService:
    def __init__(self, cfg, model_cls, restart=restart_process):
        host = cfg.get('bridge', 'host')
        port = cfg.getint('bridge', 'port')
        self.auto = cfg.get('bridge', 'heater') == 'auto'
        self.model = model_cls()
        self.restart = restart
        names = model_names(cfg, self.model)
        self.link = BridgeLink(host, port, self.on_line, self.on_connection)
        self.connected = False
        self.hello_ok = False  # STATE erst nach passendem HELLO auswerten
        self.hold = {}
        connection = 'TCP %s:%d' % (host, port)

        m = self.model
        self.sw = VeDbusService('com.victronenergy.switch.' + m.SWITCH_SERVICE, new_bus(),
                                register=False)
        self._mandatory(self.sw, connection, cfg.getint('dbus', 'switch_instance'))
        for o in m.OUTPUTS:
            self._output(o, names.get(o.name_key), names.get('group'))
        for path in m.EXTRA_PATHS:
            self.sw.add_path(m.EXTRA_PREFIX + path, None)
        self.sw.register()

        self.temps = {t.key: self._temperature(t, cfg.getint('dbus', t.instance_key),
                                               names.get(t.name_key), connection)
                      for t in m.TEMPERATURES}

    # ── D-Bus-Aufbau ────────────────────────────────────────────────────────

    def _mandatory(self, svc, connection, instance, custom_name=''):
        svc.add_mandatory_paths(__file__, VERSION, connection, instance, 0xFFFF,
                                self.model.PRODUCT, firmwareversion=VERSION,
                                hardwareversion='WomoLIN', connected=0)
        svc.add_path('/CustomName', custom_name)

    def _output(self, o, name, group):
        base = '/SwitchableOutput/%d' % o.idx
        self.sw.add_path(base + '/Name', name)
        self.sw.add_path(base + '/Status', STATUS_OFF)
        self.sw.add_path(base + '/State', 0, writeable=o.state_writeable,
                         onchangecallback=self._on_write)
        if o.dimming is not None:
            self.sw.add_path(base + '/Dimming', o.dimming, writeable=True,
                             onchangecallback=self._on_write)
        if o.measurement:
            self.sw.add_path(base + '/Measurement', None)
        self.sw.add_path(base + '/Settings/Type', o.type)
        self.sw.add_path(base + '/Settings/ValidTypes', 1 << o.type)
        self.sw.add_path(base + '/Settings/CustomName', '')
        self.sw.add_path(base + '/Settings/Group', group)
        self.sw.add_path(base + '/Settings/ShowUIControl', 1)
        for key, value in o.settings.items():
            self.sw.add_path(base + '/Settings/' + key, value)

    def _temperature(self, t, instance, custom_name, connection):
        svc = VeDbusService('com.victronenergy.temperature.' + t.service, new_bus(),
                            register=False)
        self._mandatory(svc, connection, instance, custom_name)
        svc.add_path('/Temperature', None)
        svc.add_path('/TemperatureType', t.temp_type)
        svc.add_path('/Status', 1)  # 0 = OK, 1 = nicht verbunden
        svc.register()
        return svc

    # ── Bridge → D-Bus ──────────────────────────────────────────────────────

    def on_connection(self, up):
        if not up:
            self.hello_ok = False
            self._apply(None)
        else:
            self.link.send('GET')
        return False

    def on_line(self, line):
        kind, _, rest = line.strip().partition(' ')
        if kind == 'HELLO':
            self._hello(rest)
        elif kind == 'STATE' and self.hello_ok:
            try:
                self._apply(self.model.parse_state(rest))
            except (ValueError, KeyError) as e:
                log.warning('Ungültige Zeile %r: %s', line, e)
        elif kind == 'ERR':
            log.warning('Bridge meldet Fehler: %s', rest)
        return False

    def _hello(self, rest):
        heater = parse_hello(rest)
        log.info('Bridge: %s', rest)
        if heater == self.model.TYPE:
            self.hello_ok = True
            return
        self.hello_ok = False
        self._apply(None)
        if self.auto and heater in MODELS:
            log.info('Heizungstyp %s statt %s, Neustart', heater, self.model.TYPE)
            save_type(heater)
            self.restart()
        else:
            log.error('Bridge meldet Heizung %r, eingestellt ist %r', heater, self.model.TYPE)

    def _set(self, path, value):
        if time.monotonic() < self.hold.get(path, 0):
            return
        self.sw[path] = value

    def _apply(self, s):
        self.connected = s is not None
        for svc in [self.sw, *self.temps.values()]:
            svc['/Connected'] = 1 if self.connected else 0

        if s is None:
            for svc in self.temps.values():
                svc['/Temperature'] = None
                svc['/Status'] = 1
            for o in self.model.OUTPUTS:
                self.sw['/SwitchableOutput/%d/Status' % o.idx] = STATUS_OFF
                if o.measurement:
                    self.sw['/SwitchableOutput/%d/Measurement' % o.idx] = None
            return

        for key, value in self.model.temp_values(s).items():
            self.temps[key]['/Temperature'] = value
            self.temps[key]['/Status'] = 0 if value is not None else 1
        for path, value, holdable in self.model.switch_values(s):
            if holdable:
                self._set(path, value)
            else:
                self.sw[path] = value

    # ── D-Bus → Bridge ──────────────────────────────────────────────────────

    def _on_write(self, path, value):
        if not self.connected:
            log.warning('Schreiben auf %s abgelehnt: Heizung nicht verbunden', path)
            return False
        idx = int(path.split('/')[2])
        field = path.rsplit('/', 1)[1]
        try:
            line = self.model.on_write(idx, field, value, lambda p: self.sw[p])
        except (TypeError, ValueError) as e:
            log.warning('Ungültiger Wert %r für %s: %s', value, path, e)
            return False
        if line is None:
            return False
        if line is True:
            return True

        log.info('%s = %r -> %s', path, value, line)
        if not self.link.send(line):
            return False
        self.hold[path] = time.monotonic() + self.model.HOLD_S
        return True


def load_config():
    cfg = configparser.ConfigParser()
    cfg.read_dict({
        'bridge': {'host': '192.168.1.50', 'port': '7020', 'heater': 'auto'},
        'dbus': {'switch_instance': '60', 'indoor_instance': '61', 'outdoor_instance': '62',
                 'water_instance': '62'},
    })
    cfg.read(os.path.join(HERE, 'config.ini'), encoding='utf-8')
    return cfg


def configured_type(cfg):
    heater = cfg.get('bridge', 'heater')
    if heater == 'auto':
        try:
            with open(TYPE_CACHE) as f:
                heater = f.read().strip()
        except OSError:
            heater = 'alde'
    return heater if heater in MODELS else 'alde'


def main():
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    DBusGMainLoop(set_as_default=True)
    cfg = load_config()
    heater = configured_type(cfg)
    service = HeaterService(cfg, MODELS[heater])
    service.link.start()
    log.info('dbus-lin-heater %s gestartet, Heizung: %s', VERSION, heater)
    GLib.MainLoop().run()


if __name__ == '__main__':
    main()
