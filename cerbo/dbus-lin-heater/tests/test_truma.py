import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from models import parse_hello  # noqa: E402
from models import truma as t  # noqa: E402

# Ausgabe der Firmware (siehe firmware/test/test_truma)
STATE_ARGS = 'link=1 room=25.2 sp=0.0 mode=off water=19.0 ww=0 mix=gas el=0 status=0 err=0'


class ParseTest(unittest.TestCase):
    def test_state(self):
        s = t.parse_state(STATE_ARGS)
        self.assertEqual(s.room, 25.2)
        self.assertEqual(s.water, 19.0)
        self.assertEqual(s.setpoint, 0.0)
        self.assertEqual(s.mode, 'off')
        self.assertEqual(s.mix, 'gas')
        self.assertFalse(s.heating)

    def test_missing_temperatures(self):
        s = t.parse_state(STATE_ARGS.replace('room=25.2', 'room=-'))
        self.assertIsNone(s.room)

    def test_link_lost(self):
        self.assertIsNone(t.parse_state('link=0'))

    def test_hello(self):
        self.assertEqual(parse_hello('lin-bridge 1.1 heater=truma'), 'truma')
        self.assertIsNone(parse_hello('lin-bridge 1.1'))


class WriteTest(unittest.TestCase):
    def setUp(self):
        self.m = t.Model()

    def write(self, idx, field, value):
        return self.m.on_write(idx, field, value, lambda p: None)

    def test_setpoint_rounds_and_clamps(self):
        self.assertEqual(self.write(t.OUT_ROOM, 'Dimming', 21.6), 'SET sp=22')
        self.assertEqual(self.write(t.OUT_ROOM, 'Dimming', 40), 'SET sp=30')
        self.assertEqual(self.write(t.OUT_ROOM, 'Dimming', 1), 'SET sp=5')

    def test_heating_on_uses_last_mode(self):
        self.m.switch_values(t.parse_state(STATE_ARGS.replace('sp=0.0 mode=off',
                                                              'sp=21.0 mode=boost')))
        self.assertEqual(self.write(t.OUT_ROOM, 'State', 0), 'SET mode=off')
        self.assertEqual(self.write(t.OUT_ROOM, 'State', 1), 'SET mode=boost')

    def test_dropdowns(self):
        self.assertEqual(self.write(t.OUT_WATER, 'Dimming', 1), 'SET ww=40')
        self.assertEqual(self.write(t.OUT_MIX, 'Dimming', 2), 'SET mix=el')
        self.assertEqual(self.write(t.OUT_ELECTRIC, 'Dimming', 0), 'SET el=900')
        with self.assertRaises(ValueError):
            self.write(t.OUT_MODE, 'Dimming', 2)
        self.assertIsNone(self.write(t.OUT_WATER, 'State', 1))


if __name__ == '__main__':
    unittest.main()
