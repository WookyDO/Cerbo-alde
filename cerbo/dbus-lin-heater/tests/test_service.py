"""Tests für dbus_lin_heater.HeaterService mit Attrappen für dbus, GLib und vedbus."""

import os
import sys
import tempfile
import types
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))


class FakeVeDbusService:
    def __init__(self, name, bus=None, register=None):
        self.name = name
        self.values = {}
        self.callbacks = {}
        self.registered = False

    def add_mandatory_paths(self, processname, processversion, connection, deviceinstance,
                            productid, productname, firmwareversion, hardwareversion,
                            connected):
        for k, v in (('/Mgmt/Connection', connection), ('/DeviceInstance', deviceinstance),
                     ('/ProductName', productname), ('/Connected', connected)):
            self.add_path(k, v)

    def add_path(self, path, value, description='', writeable=False, onchangecallback=None,
                 **kwargs):
        assert path not in self.values, path
        self.values[path] = value
        if onchangecallback:
            self.callbacks[path] = onchangecallback

    def register(self):
        self.registered = True

    def __getitem__(self, path):
        return self.values[path]

    def __setitem__(self, path, value):
        assert path in self.values, path
        self.values[path] = value

    def write(self, path, value):
        """Simuliert einen Schreibzugriff aus der GUI."""
        if self.callbacks[path](path, value):
            self.values[path] = value
            return True
        return False


def install_fakes():
    dbus = types.ModuleType('dbus')
    dbus.SystemBus = dbus.SessionBus = lambda private=False: object()
    mainloop = types.ModuleType('dbus.mainloop')
    glib_loop = types.ModuleType('dbus.mainloop.glib')
    glib_loop.DBusGMainLoop = lambda set_as_default=False: None
    gi = types.ModuleType('gi')
    repo = types.ModuleType('gi.repository')
    repo.GLib = types.SimpleNamespace(idle_add=lambda f, *a: f(*a))
    vedbus = types.ModuleType('vedbus')
    vedbus.VeDbusService = FakeVeDbusService
    sys.modules.update({'dbus': dbus, 'dbus.mainloop': mainloop, 'dbus.mainloop.glib': glib_loop,
                        'gi': gi, 'gi.repository': repo, 'vedbus': vedbus})


install_fakes()
import dbus_lin_heater as svcmod  # noqa: E402
from models import MODELS, alde, truma  # noqa: E402

ALDE_STATE = ('STATE link=1 z1=21.0 z2=- out=8.0 sp=20.0 gas=1 valve=1 el=2 ww=1 '
              'on=1 busy=0 err=0 ac=1 pump=1 raw=DE9E29')
TRUMA_STATE = ('STATE link=1 room=19.5 sp=21.0 mode=eco water=48.0 ww=60 mix=gas el=0 '
               'status=5 err=0')
OUT = '/SwitchableOutput/%d/%s'


class Base(unittest.TestCase):
    MODEL = alde.Model
    HELLO = 'HELLO lin-bridge 1.1 heater=alde'

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        svcmod.TYPE_CACHE = os.path.join(self.tmp.name, 'heater.type')
        self.restarted = []
        self.svc = svcmod.HeaterService(svcmod.load_config(), self.MODEL,
                                        restart=lambda: self.restarted.append(True))
        self.sent = []
        self.svc.link.send = lambda line: self.sent.append(line) or True
        self.sw = self.svc.sw
        self.svc.on_line(self.HELLO)

    def tearDown(self):
        self.tmp.cleanup()


class AldeServiceTest(Base):
    def test_registered_with_gui_types(self):
        self.assertTrue(self.sw.registered)
        self.assertEqual(self.sw.name, 'com.victronenergy.switch.alde_3030')
        self.assertEqual(self.sw[OUT % (0, 'Settings/Type')], 3)
        self.assertEqual(self.sw[OUT % (0, 'Settings/ValidTypes')], 1 << 3)
        self.assertEqual(self.sw[OUT % (2, 'Settings/DimmingMax')], 3)
        self.assertEqual(self.sw[OUT % (3, 'Settings/Labels')], ['Aus', 'An', 'Boost'])
        self.assertEqual(self.sw[OUT % (0, 'Name')], 'Raumtemperatur')
        self.assertEqual(self.svc.temps['indoor']['/TemperatureType'], 3)
        self.assertEqual(self.svc.temps['outdoor']['/TemperatureType'], 4)
        self.assertEqual(self.svc.temps['outdoor'].name, 'com.victronenergy.temperature.alde_aussen')

    def test_state_is_published(self):
        self.svc.on_line(ALDE_STATE)
        self.assertEqual(self.sw['/Connected'], 1)
        self.assertEqual(self.sw[OUT % (0, 'Dimming')], 20.0)
        self.assertEqual(self.sw[OUT % (0, 'Measurement')], 21.0)
        self.assertEqual(self.sw[OUT % (0, 'State')], 1)
        self.assertEqual(self.sw[OUT % (1, 'State')], 1)
        self.assertEqual(self.sw[OUT % (2, 'State')], 1)
        self.assertEqual(self.sw[OUT % (2, 'Dimming')], 2)
        self.assertEqual(self.sw[OUT % (3, 'Dimming')], 1)
        self.assertEqual(self.sw[OUT % (1, 'Status')], 0x09)
        self.assertEqual(self.svc.temps['indoor']['/Temperature'], 21.0)
        self.assertEqual(self.svc.temps['indoor']['/Status'], 0)
        self.assertEqual(self.svc.temps['outdoor']['/Temperature'], 8.0)
        self.assertEqual(self.sw['/Alde/AcInput'], 1)

    def test_link_lost(self):
        self.svc.on_line(ALDE_STATE)
        self.svc.on_line('STATE link=0')
        self.assertEqual(self.sw['/Connected'], 0)
        self.assertIsNone(self.svc.temps['indoor']['/Temperature'])
        self.assertEqual(self.svc.temps['indoor']['/Status'], 1)
        self.assertFalse(self.sw.write(OUT % (1, 'State'), 0))
        self.assertEqual(self.sent, [])

    def test_writes_are_forwarded(self):
        self.svc.on_line(ALDE_STATE)
        self.assertTrue(self.sw.write(OUT % (0, 'Dimming'), 21.3))
        self.assertTrue(self.sw.write(OUT % (1, 'State'), 0))
        self.assertTrue(self.sw.write(OUT % (3, 'Dimming'), 2))
        self.assertEqual(self.sent, ['SET sp=21.5', 'SET gas=0', 'SET ww=2'])

    def test_electric_stepped_switch(self):
        self.svc.on_line(ALDE_STATE)
        self.assertTrue(self.sw.write(OUT % (2, 'Dimming'), 3))  # an: Stufe sofort
        self.assertTrue(self.sw.write(OUT % (2, 'State'), 0))
        self.assertTrue(self.sw.write(OUT % (2, 'Dimming'), 1))  # aus: nur vormerken
        self.assertTrue(self.sw.write(OUT % (2, 'State'), 1))
        self.assertEqual(self.sent, ['SET el=3', 'SET el=0', 'SET el=1'])

    def test_stale_state_does_not_overwrite_recent_write(self):
        self.svc.on_line(ALDE_STATE)
        self.sw.write(OUT % (0, 'Dimming'), 23.0)
        self.svc.on_line(ALDE_STATE)  # Panel meldet noch 20 °C
        self.assertEqual(self.sw[OUT % (0, 'Dimming')], 23.0)
        self.svc.hold.clear()
        self.svc.on_line(ALDE_STATE)
        self.assertEqual(self.sw[OUT % (0, 'Dimming')], 20.0)

    def test_read_only_and_invalid_writes_rejected(self):
        self.svc.on_line(ALDE_STATE)
        self.assertFalse(self.sw.write(OUT % (0, 'State'), 0))
        self.assertFalse(self.sw.write(OUT % (3, 'Dimming'), 7))
        self.assertEqual(self.sent, [])

    def test_hello_without_type_is_not_trusted(self):
        self.svc.on_line('HELLO lin-bridge 1.1')
        self.svc.on_line(ALDE_STATE)
        self.assertEqual(self.sw['/Connected'], 0)
        self.assertEqual(self.restarted, [])


class TrumaServiceTest(Base):
    MODEL = truma.Model
    HELLO = 'HELLO lin-bridge 1.1 heater=truma'

    def test_registered(self):
        self.assertEqual(self.sw.name, 'com.victronenergy.switch.truma_combi')
        self.assertEqual(self.sw[OUT % (0, 'Settings/StepSize')], 1)
        self.assertEqual(self.sw[OUT % (2, 'Settings/Labels')], ['Aus', 'Eco', 'Heiß', 'Boost'])
        self.assertEqual(self.sw[OUT % (4, 'Settings/Labels')], ['900 W', '1800 W'])
        self.assertEqual(self.sw[OUT % (1, 'Name')], 'Heizstufe')
        self.assertEqual(self.svc.temps['water']['/TemperatureType'], 5)
        self.assertEqual(self.svc.temps['water'].name, 'com.victronenergy.temperature.truma_wasser')
        self.assertEqual(self.svc.temps['water']['/DeviceInstance'], 62)

    def test_state_is_published(self):
        self.svc.on_line(TRUMA_STATE)
        self.assertEqual(self.sw['/Connected'], 1)
        self.assertEqual(self.sw[OUT % (0, 'State')], 1)
        self.assertEqual(self.sw[OUT % (0, 'Dimming')], 21.0)
        self.assertEqual(self.sw[OUT % (0, 'Measurement')], 19.5)
        self.assertEqual(self.sw[OUT % (1, 'Dimming')], 0)  # Eco
        self.assertEqual(self.sw[OUT % (2, 'Dimming')], 2)  # Heiß
        self.assertEqual(self.sw[OUT % (3, 'Dimming')], 0)  # Gas
        self.assertEqual(self.sw[OUT % (4, 'Status')], 0)
        self.assertEqual(self.svc.temps['water']['/Temperature'], 48.0)
        self.assertEqual(self.sw['/Truma/OperatingStatusText'], 'An')

    def test_off_keeps_last_setpoint_visible(self):
        self.svc.on_line(TRUMA_STATE)
        self.svc.on_line(TRUMA_STATE.replace('sp=21.0 mode=eco', 'sp=0.0 mode=off'))
        self.assertEqual(self.sw[OUT % (0, 'State')], 0)
        self.assertEqual(self.sw[OUT % (0, 'Dimming')], 21.0)

    def test_writes_are_forwarded(self):
        self.svc.on_line(TRUMA_STATE)
        self.assertTrue(self.sw.write(OUT % (0, 'Dimming'), 22.4))
        self.assertTrue(self.sw.write(OUT % (0, 'State'), 0))
        self.assertTrue(self.sw.write(OUT % (0, 'State'), 1))
        self.assertTrue(self.sw.write(OUT % (1, 'Dimming'), 1))
        self.assertTrue(self.sw.write(OUT % (2, 'Dimming'), 3))
        self.assertTrue(self.sw.write(OUT % (3, 'Dimming'), 1))
        self.assertTrue(self.sw.write(OUT % (4, 'Dimming'), 1))
        self.assertEqual(self.sent, ['SET sp=22', 'SET mode=off', 'SET mode=eco', 'SET mode=boost',
                                     'SET ww=200', 'SET mix=mix', 'SET el=1800'])

    def test_invalid_writes_rejected(self):
        self.svc.on_line(TRUMA_STATE)
        self.assertFalse(self.sw.write(OUT % (2, 'Dimming'), 4))
        self.assertFalse(self.sw.write(OUT % (3, 'State'), 0))
        self.assertEqual(self.sent, [])

    def test_hold_lasts_until_cp_plus_confirms(self):
        self.svc.on_line(TRUMA_STATE)
        self.sw.write(OUT % (2, 'Dimming'), 0)
        self.svc.on_line(TRUMA_STATE)  # CP Plus meldet noch "heiß"
        self.assertEqual(self.sw[OUT % (2, 'Dimming')], 0)
        self.assertGreaterEqual(self.svc.hold[OUT % (2, 'Dimming')] - svcmod.time.monotonic(), 30)


class HelloTest(Base):
    def test_state_ignored_before_hello(self):
        svc = svcmod.HeaterService(svcmod.load_config(), alde.Model, restart=lambda: None)
        svc.on_line(ALDE_STATE)
        self.assertEqual(svc.sw['/Connected'], 0)

    def test_other_type_restarts_and_remembers(self):
        self.svc.on_line(ALDE_STATE)
        self.svc.on_line('HELLO lin-bridge 1.1 heater=truma')
        self.assertEqual(self.restarted, [True])
        self.assertEqual(self.sw['/Connected'], 0)
        self.assertEqual(svcmod.configured_type(svcmod.load_config()), 'truma')
        self.svc.on_line(TRUMA_STATE)  # bis zum Neustart nichts auswerten
        self.assertEqual(self.sw['/Connected'], 0)

    def test_fixed_type_does_not_restart(self):
        cfg = svcmod.load_config()
        cfg.set('bridge', 'heater', 'alde')
        svc = svcmod.HeaterService(cfg, alde.Model, restart=lambda: self.restarted.append(True))
        svc.on_line('HELLO lin-bridge 1.1 heater=truma')
        self.assertEqual(self.restarted, [])

    def test_names_per_type(self):
        cfg = svcmod.load_config()
        cfg.read_dict({'names.alde': {'group': 'Heizung Bus'}})
        self.assertEqual(svcmod.model_names(cfg, alde.Model())['group'], 'Heizung Bus')
        self.assertEqual(svcmod.model_names(cfg, truma.Model())['group'], 'Truma Heizung')

    def test_models_registered(self):
        self.assertEqual(sorted(MODELS), ['alde', 'truma'])


if __name__ == '__main__':
    unittest.main()
