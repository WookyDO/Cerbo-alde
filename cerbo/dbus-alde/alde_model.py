"""Zeilenprotokoll der Alde-LIN-Bridge und Abbildung auf Venus-OS-Werte.

Ohne D-Bus-Abhängigkeiten, damit es auf dem PC testbar ist.
"""

from dataclasses import dataclass
from typing import Optional

SETPOINT_MIN = 5.0
SETPOINT_MAX = 30.0
ELECTRIC_MAX = 3
WATER_LABELS = ['Aus', 'An', 'Boost']


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


def parse_line(line):
    """Liefert (art, daten). STATE -> HeaterState oder None (Panel nicht erreichbar)."""
    line = line.strip()
    kind, _, rest = line.partition(' ')
    if kind == 'STATE':
        return kind, parse_state(rest)
    return kind, rest


def parse_state(args):
    kv = dict(tok.split('=', 1) for tok in args.split() if '=' in tok)
    if kv.get('link') != '1':
        return None

    def temp(key):
        v = kv.get(key, '-')
        return None if v == '-' else float(v)

    def flag(key):
        return kv.get(key) == '1'

    return HeaterState(
        zone1=temp('z1'), zone2=temp('z2'), outdoor=temp('out'),
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
