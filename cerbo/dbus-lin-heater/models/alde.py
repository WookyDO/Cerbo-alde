"""Alde Compact 3030 Plus.

  /SwitchableOutput/0  Temperatur-Sollwert (Typ 3), Messwert = Innentemperatur
  /SwitchableOutput/1  Gas ein/aus (Typ 1)
  /SwitchableOutput/2  Elektro 1/2/3 kW (Stufenschalter, Typ 4)
  /SwitchableOutput/3  Warmwasser Aus/An/Boost (Dropdown, Typ 6)
  temperature.alde_innen / alde_aussen
"""

from dataclasses import dataclass
from typing import Optional

from .common import (STATUS_OFF, STATUS_ON, TEMP_OUTDOOR, TEMP_ROOM, TYPE_DROPDOWN,
                     TYPE_SETPOINT, TYPE_STEPPED, TYPE_TOGGLE, Output, Temperature,
                     split_state, temp)

SETPOINT_MIN = 5.0
SETPOINT_MAX = 30.0
ELECTRIC_MAX = 3
WATER_LABELS = ['Aus', 'An', 'Boost']

OUT_SETPOINT, OUT_GAS, OUT_ELECTRIC, OUT_WATER = range(4)
OUT = '/SwitchableOutput/%d/%s'


@dataclass
class HeaterState:
    zone1: Optional[float]
    zone2: Optional[float]
    outdoor: Optional[float]
    setpoint: float
    gas: bool
    valve: bool
    electric: int
    water: int
    panel_on: bool
    busy: bool
    error: bool
    ac: bool
    pump: bool
    raw: str = ''

    @property
    def heating(self):
        return self.gas or self.electric > 0


def parse_state(args):
    """STATE-Argumente -> HeaterState oder None (Panel nicht erreichbar)."""
    kv = split_state(args)
    if kv is None:
        return None

    def flag(key):
        return kv.get(key) == '1'

    return HeaterState(
        zone1=temp(kv, 'z1'), zone2=temp(kv, 'z2'), outdoor=temp(kv, 'out'),
        setpoint=float(kv['sp']), gas=flag('gas'), valve=flag('valve'),
        electric=int(kv['el']), water=int(kv['ww']), panel_on=flag('on'),
        busy=flag('busy'), error=flag('err'), ac=flag('ac'), pump=flag('pump'),
        raw=kv.get('raw', ''))


def round_setpoint(value):
    value = min(max(float(value), SETPOINT_MIN), SETPOINT_MAX)
    return round(value * 2) / 2


def format_set(setpoint=None, gas=None, electric=None, water=None):
    """Baut eine SET-Zeile; ValueError bei ungültigen Werten."""
    parts = []
    if setpoint is not None:
        parts.append('sp=%.1f' % round_setpoint(setpoint))
    if gas is not None:
        parts.append('gas=%d' % (1 if gas else 0))
    if electric is not None:
        if not 0 <= int(electric) <= ELECTRIC_MAX:
            raise ValueError('electric 0..3')
        parts.append('el=%d' % int(electric))
    if water is not None:
        if not 0 <= int(water) < len(WATER_LABELS):
            raise ValueError('water 0..2')
        parts.append('ww=%d' % int(water))
    if not parts:
        raise ValueError('keine Werte')
    return 'SET ' + ' '.join(parts)


# Elektro als Stufenschalter der GUI: /State = an/aus, /Dimming = Stufe 1..3.

def electric_to_switch(electric, last_level):
    """kW-Stufe -> (State, Dimming). Bei 'aus' bleibt die letzte Stufe sichtbar."""
    if electric > 0:
        return 1, electric
    return 0, last_level if 1 <= last_level <= ELECTRIC_MAX else 1


def switch_to_electric(state, level):
    """(State, Dimming) -> kW-Stufe 0..3."""
    if not state:
        return 0
    return min(max(int(level), 1), ELECTRIC_MAX)


class Model:
    TYPE = 'alde'
    PRODUCT = 'Alde Compact 3030 Plus'
    SWITCH_SERVICE = 'alde_3030'
    EXTRA_PREFIX = '/Alde/'
    EXTRA_PATHS = ('PanelOn', 'Pump', 'GasValve', 'AcInput', 'Error', 'Busy',
                   'OutdoorTemperature', 'Raw')
    HOLD_S = 3.0  # das Panel bestätigt Befehle innerhalb von Sekunden
    NAMES = {'group': 'Alde Heizung', 'setpoint': 'Raumtemperatur', 'gas': 'Gas',
             'electric': 'Elektro (kW)', 'water': 'Warmwasser',
             'indoor': 'Alde Innen', 'outdoor': 'Alde Aussen'}

    OUTPUTS = (
        Output(OUT_SETPOINT, 'setpoint', TYPE_SETPOINT, dimming=20.0, measurement=True,
               settings={'DimmingMin': SETPOINT_MIN, 'DimmingMax': SETPOINT_MAX,
                         'StepSize': 0.5}),
        Output(OUT_GAS, 'gas', TYPE_TOGGLE, state_writeable=True),
        Output(OUT_ELECTRIC, 'electric', TYPE_STEPPED, state_writeable=True, dimming=1,
               settings={'DimmingMax': ELECTRIC_MAX}),
        Output(OUT_WATER, 'water', TYPE_DROPDOWN, dimming=0, settings={'Labels': WATER_LABELS}),
    )
    TEMPERATURES = (
        Temperature('indoor', 'alde_innen', 'indoor_instance', 'indoor', TEMP_ROOM),
        Temperature('outdoor', 'alde_aussen', 'outdoor_instance', 'outdoor', TEMP_OUTDOOR),
    )

    def __init__(self):
        self.electric_level = 1

    parse_state = staticmethod(parse_state)

    def temp_values(self, s):
        return {'indoor': s.zone1, 'outdoor': s.outdoor}

    def switch_values(self, s):
        """(Pfad, Wert, hold) – hold: nach einer GUI-Eingabe kurz nicht überschreiben."""
        if s.electric > 0:
            self.electric_level = s.electric
        el_state, el_level = electric_to_switch(s.electric, self.electric_level)
        values = [
            (OUT % (OUT_SETPOINT, 'State'), 1 if s.heating else 0, False),
            (OUT % (OUT_SETPOINT, 'Measurement'), s.zone1, False),
            (OUT % (OUT_SETPOINT, 'Dimming'), s.setpoint, True),
            (OUT % (OUT_GAS, 'State'), 1 if s.gas else 0, True),
            (OUT % (OUT_ELECTRIC, 'State'), el_state, True),
            (OUT % (OUT_ELECTRIC, 'Dimming'), el_level, True),
            (OUT % (OUT_WATER, 'State'), 1 if s.water else 0, False),
            (OUT % (OUT_WATER, 'Dimming'), s.water, True),
        ]
        for idx, on in ((OUT_SETPOINT, s.heating), (OUT_GAS, s.gas),
                        (OUT_ELECTRIC, s.electric > 0), (OUT_WATER, s.water > 0)):
            values.append((OUT % (idx, 'Status'), STATUS_ON if on else STATUS_OFF, False))
        extra = {'PanelOn': int(s.panel_on), 'Pump': int(s.pump), 'GasValve': int(s.valve),
                 'AcInput': int(s.ac), 'Error': int(s.error), 'Busy': int(s.busy),
                 'OutdoorTemperature': s.outdoor, 'Raw': s.raw}
        values += [(self.EXTRA_PREFIX + k, v, False) for k, v in extra.items()]
        return values

    def on_write(self, idx, field, value, read):
        """GUI-Eingabe -> SET-Zeile, True (lokal erledigt) oder None (abgelehnt).

        read(pfad) liefert den aktuellen Wert eines anderen Pfads."""
        if idx == OUT_SETPOINT and field == 'Dimming':
            return format_set(setpoint=value)
        if idx == OUT_GAS and field == 'State':
            return format_set(gas=bool(value))
        if idx == OUT_ELECTRIC:
            state = value if field == 'State' else read(OUT % (OUT_ELECTRIC, 'State'))
            level = value if field == 'Dimming' else read(OUT % (OUT_ELECTRIC, 'Dimming'))
            if field == 'Dimming':
                self.electric_level = switch_to_electric(1, level)
                if not state:
                    return True  # nur Stufe vormerken, Elektro bleibt aus
            return format_set(electric=switch_to_electric(state, level))
        if idx == OUT_WATER and field == 'Dimming':
            return format_set(water=int(value))
        return None
