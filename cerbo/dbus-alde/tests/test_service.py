"""Tests für dbus_alde.AldeService mit Attrappen für dbus, GLib und vedbus."""

import os
import sys
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
import dbus_alde  # noqa: E402

STATE = ('STATE link=1 z1=21.0 z2=- out=8.0 sp=20.0 gas=1 valve=1 el=2 ww=1 '
         'on=1 busy=0 err=0 ac=1 pump=1 raw=DE9E29')
OUT = '/SwitchableOutput/%d/%s'


class ServiceTest(unittest.TestCase):
    def setUp(self):
        self.svc = dbus_alde.AldeService(dbus_alde.load_config())
        self.sent = []
        self.svc.link.send = lambda line: self.sent.append(line) or True
        self.sw = self.svc.sw

    def test_registered_with_gui_types(self):
        self.assertTrue(self.sw.registered)
        self.assertEqual(self.sw[OUT % (0, 'Settings/Type')], 3)
        self.assertEqual(self.sw[OUT % (0, 'Settings/ValidTypes')], 1 << 3)
        self.assertEqual(self.sw[OUT % (2, 'Settings/DimmingMax')], 3)
        self.assertEqual(self.sw[OUT % (3, 'Settings/Labels')], ['Aus', 'An', 'Boost'])
        self.assertEqual(self.svc.indoor['/TemperatureType'], 3)
        self.assertEqual(self.svc.outdoor['/TemperatureType'], 4)

    def test_state_is_published(self):
        self.svc.on_line(STATE)
        self.assertEqual(self.sw['/Connected'], 1)
        self.assertEqual(self.sw[OUT % (0, 'Dimming')], 20.0)
        self.assertEqual(self.sw[OUT % (0, 'Measurement')], 21.0)
        self.assertEqual(self.sw[OUT % (0, 'State')], 1)
        self.assertEqual(self.sw[OUT % (1, 'State')], 1)
        self.assertEqual(self.sw[OUT % (2, 'State')], 1)
        self.assertEqual(self.sw[OUT % (2, 'Dimming')], 2)
        self.assertEqual(self.sw[OUT % (3, 'Dimming')], 1)
        self.assertEqual(self.sw[OUT % (1, 'Status')], 0x09)
        self.assertEqual(self.svc.indoor['/Temperature'], 21.0)
        self.assertEqual(self.svc.indoor['/Status'], 0)
        self.assertEqual(self.svc.outdoor['/Temperature'], 8.0)
        self.assertEqual(self.sw['/Alde/AcInput'], 1)

    def test_link_lost(self):
        self.svc.on_line(STATE)
        self.svc.on_line('STATE link=0')
        self.assertEqual(self.sw['/Connected'], 0)
        self.assertIsNone(self.svc.indoor['/Temperature'])
        self.assertEqual(self.svc.indoor['/Status'], 1)
        self.assertFalse(self.sw.write(OUT % (1, 'State'), 0))
        self.assertEqual(self.sent, [])

    def test_writes_are_forwarded(self):
        self.svc.on_line(STATE)
        self.assertTrue(self.sw.write(OUT % (0, 'Dimming'), 21.3))
        self.assertTrue(self.sw.write(OUT % (1, 'State'), 0))
        self.assertTrue(self.sw.write(OUT % (3, 'Dimming'), 2))
        self.assertEqual(self.sent, ['SET sp=21.5', 'SET gas=0', 'SET ww=2'])

    def test_electric_stepped_switch(self):
        self.svc.on_line(STATE)
        self.assertTrue(self.sw.write(OUT % (2, 'Dimming'), 3))  # an: Stufe sofort
        self.assertTrue(self.sw.write(OUT % (2, 'State'), 0))
        self.assertTrue(self.sw.write(OUT % (2, 'Dimming'), 1))  # aus: nur vormerken
        self.assertTrue(self.sw.write(OUT % (2, 'State'), 1))
        self.assertEqual(self.sent, ['SET el=3', 'SET el=0', 'SET el=1'])

    def test_stale_state_does_not_overwrite_recent_write(self):
        self.svc.on_line(STATE)
        self.sw.write(OUT % (0, 'Dimming'), 23.0)
        self.svc.on_line(STATE)  # Panel meldet noch 20 °C
        self.assertEqual(self.sw[OUT % (0, 'Dimming')], 23.0)
        self.svc.hold.clear()
        self.svc.on_line(STATE)
        self.assertEqual(self.sw[OUT % (0, 'Dimming')], 20.0)

    def test_read_only_and_invalid_writes_rejected(self):
        self.svc.on_line(STATE)
        self.assertFalse(self.sw.write(OUT % (0, 'State'), 0))
        self.assertFalse(self.sw.write(OUT % (3, 'Dimming'), 7))
        self.assertEqual(self.sent, [])


if __name__ == '__main__':
    unittest.main()
