import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from models import alde as m  # noqa: E402

# Ausgabe der Firmware (siehe firmware/test/test_protocol)
STATE_LINE = ('STATE link=1 z1=21.0 z2=- out=8.0 sp=20.0 gas=1 valve=1 el=2 ww=1 '
              'on=1 busy=0 err=0 ac=0 pump=0 raw=DE9E09')


class ParseTest(unittest.TestCase):
    def test_state(self):
        s = m.parse_state(STATE_LINE.split(' ', 1)[1] + '\n')
        self.assertEqual(s.zone1, 21.0)
        self.assertIsNone(s.zone2)
        self.assertEqual(s.outdoor, 8.0)
        self.assertEqual(s.setpoint, 20.0)
        self.assertTrue(s.gas and s.valve and s.panel_on)
        self.assertEqual(s.electric, 2)
        self.assertEqual(s.water, 1)
        self.assertFalse(s.error)
        self.assertTrue(s.heating)
        self.assertEqual(s.raw, 'DE9E09')

    def test_link_lost(self):
        self.assertIsNone(m.parse_state('link=0'))


class SetTest(unittest.TestCase):
    def test_format(self):
        self.assertEqual(m.format_set(setpoint=21.3, gas=False, electric=3, water=2),
                         'SET sp=21.5 gas=0 el=3 ww=2')
        self.assertEqual(m.format_set(setpoint=45), 'SET sp=30.0')
        self.assertEqual(m.format_set(setpoint=1), 'SET sp=5.0')

    def test_invalid(self):
        with self.assertRaises(ValueError):
            m.format_set()
        with self.assertRaises(ValueError):
            m.format_set(electric=4)
        with self.assertRaises(ValueError):
            m.format_set(water=3)


class ElectricTest(unittest.TestCase):
    def test_roundtrip(self):
        self.assertEqual(m.electric_to_switch(2, 1), (1, 2))
        self.assertEqual(m.electric_to_switch(0, 3), (0, 3))
        self.assertEqual(m.electric_to_switch(0, 0), (0, 1))
        self.assertEqual(m.switch_to_electric(0, 3), 0)
        self.assertEqual(m.switch_to_electric(1, 3), 3)
        self.assertEqual(m.switch_to_electric(1, 0), 1)


if __name__ == '__main__':
    unittest.main()
