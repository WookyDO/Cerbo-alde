"""Truma Combi mit CP Plus (die Bridge ist dort iNet Box).

  /SwitchableOutput/0  Raumtemperatur (Typ 3), Dimming = Soll
  /SwitchableOutput/1  Heizstufe Aus/Eco/Boost (Dropdown) – der Temperaturregler
                       der GUI v2 hat keinen Ausschalter, daher hier "Aus"
  /SwitchableOutput/2  Warmwasser Aus/Eco/Heiß/Boost (Dropdown)
  /SwitchableOutput/3  Energie Gas/Mix/Elektro (Dropdown)
  /SwitchableOutput/4  Elektroleistung 900/1800 W (Dropdown)
  temperature.truma_innen / truma_wasser

Die Regeln des CP Plus (Sollwert + Heizstufe, Energie + Leistung gehören
zusammen) setzt die Firmware um; hier wird nur übersetzt.
"""

from dataclasses import dataclass
from typing import Optional

from .common import (STATUS_OFF, STATUS_ON, TEMP_ROOM, TEMP_WATER_HEATER, TYPE_DROPDOWN,
                     TYPE_SETPOINT, Output, Temperature, split_state, temp)

ROOM_MIN, ROOM_MAX = 5, 30
MODES = ['off', 'eco', 'boost']
MODE_LABELS = ['Aus', 'Eco', 'Boost']
WATER_VALUES = [0, 40, 60, 200]
WATER_LABELS = ['Aus', 'Eco', 'Heiß', 'Boost']
MIXES = ['gas', 'mix', 'el']
MIX_LABELS = ['Gas', 'Mix', 'Elektro']
EL_VALUES = [900, 1800]
EL_LABELS = ['900 W', '1800 W']
OPERATING = {0: 'Aus', 1: 'Warnung', 4: 'Startet', 5: 'An'}

OUT_ROOM, OUT_MODE, OUT_WATER, OUT_MIX, OUT_ELECTRIC = range(5)
OUT = '/SwitchableOutput/%d/%s'


@dataclass
class TrumaState:
    room: Optional[float]
    setpoint: float      # 0 = Heizung aus
    mode: str            # off, eco, boost
    water: Optional[float]
    water_target: int    # 0, 40, 60, 200
    mix: str             # gas, mix, el
    electric: int        # 0, 900, 1800 W
    status: int
    error: int

    @property
    def heating(self):
        return self.mode != 'off' and self.setpoint >= ROOM_MIN


def parse_state(args):
    kv = split_state(args)
    if kv is None:
        return None
    return TrumaState(
        room=temp(kv, 'room'), setpoint=float(kv['sp']), mode=kv['mode'],
        water=temp(kv, 'water'), water_target=int(kv['ww']), mix=kv['mix'],
        electric=int(kv['el']), status=int(kv['status']), error=int(kv['err']))


def _index(values, value, default=0):
    return values.index(value) if value in values else default


def _pick(values, index):
    i = int(index)
    if not 0 <= i < len(values):
        raise ValueError('Auswahl %d ungültig' % i)
    return values[i]


class Model:
    TYPE = 'truma'
    PRODUCT = 'Truma Combi (CP Plus)'
    SWITCH_SERVICE = 'truma_combi'
    EXTRA_PREFIX = '/Truma/'
    EXTRA_PATHS = ('OperatingStatus', 'OperatingStatusText', 'ErrorCode', 'WaterTarget')
    # Das CP Plus holt Befehle selbst ab und meldet sich erst nach bis zu 30 s.
    HOLD_S = 40.0
    NAMES = {'group': 'Truma Heizung', 'setpoint': 'Raumtemperatur', 'mode': 'Heizstufe',
             'water': 'Warmwasser', 'mix': 'Energie', 'electric': 'Elektro',
             'indoor': 'Truma Innen', 'water_temp': 'Truma Wasser'}

    OUTPUTS = (
        Output(OUT_ROOM, 'setpoint', TYPE_SETPOINT, state_writeable=True, dimming=20.0,
               measurement=True,
               settings={'DimmingMin': ROOM_MIN, 'DimmingMax': ROOM_MAX, 'StepSize': 1}),
        Output(OUT_MODE, 'mode', TYPE_DROPDOWN, dimming=0, settings={'Labels': MODE_LABELS}),
        Output(OUT_WATER, 'water', TYPE_DROPDOWN, dimming=0, settings={'Labels': WATER_LABELS}),
        Output(OUT_MIX, 'mix', TYPE_DROPDOWN, dimming=0, settings={'Labels': MIX_LABELS}),
        Output(OUT_ELECTRIC, 'electric', TYPE_DROPDOWN, dimming=0,
               settings={'Labels': EL_LABELS}),
    )
    TEMPERATURES = (
        Temperature('indoor', 'truma_innen', 'indoor_instance', 'indoor', TEMP_ROOM),
        Temperature('water', 'truma_wasser', 'water_instance', 'water_temp', TEMP_WATER_HEATER),
    )

    def __init__(self):
        self.last_setpoint = 20.0
        self.last_mode = 'eco'
        self.last_electric = 900

    parse_state = staticmethod(parse_state)

    def temp_values(self, s):
        return {'indoor': s.room, 'water': s.water}

    def switch_values(self, s):
        # Ist etwas aus, zeigt die GUI die zuletzt gewählte Einstellung.
        if s.setpoint >= ROOM_MIN:
            self.last_setpoint = s.setpoint
        if s.mode in ('eco', 'boost'):
            self.last_mode = s.mode
        if s.electric in EL_VALUES:
            self.last_electric = s.electric
        values = [
            (OUT % (OUT_ROOM, 'State'), 1 if s.heating else 0, True),
            (OUT % (OUT_ROOM, 'Measurement'), s.room, False),
            (OUT % (OUT_ROOM, 'Dimming'), self.last_setpoint, True),
            (OUT % (OUT_MODE, 'State'), 1 if s.heating else 0, False),
            (OUT % (OUT_MODE, 'Dimming'), _index(MODES, s.mode), True),
            (OUT % (OUT_WATER, 'State'), 1 if s.water_target else 0, False),
            (OUT % (OUT_WATER, 'Dimming'), _index(WATER_VALUES, s.water_target), True),
            (OUT % (OUT_MIX, 'State'), 1, False),
            (OUT % (OUT_MIX, 'Dimming'), _index(MIXES, s.mix), True),
            (OUT % (OUT_ELECTRIC, 'State'), 1 if s.electric else 0, False),
            (OUT % (OUT_ELECTRIC, 'Dimming'), _index(EL_VALUES, self.last_electric), True),
        ]
        for idx, on in ((OUT_ROOM, s.heating), (OUT_MODE, s.heating),
                        (OUT_WATER, s.water_target > 0), (OUT_MIX, True),
                        (OUT_ELECTRIC, s.electric > 0)):
            values.append((OUT % (idx, 'Status'), STATUS_ON if on else STATUS_OFF, False))
        extra = {'OperatingStatus': s.status,
                 'OperatingStatusText': OPERATING.get(s.status, str(s.status)),
                 'ErrorCode': s.error, 'WaterTarget': s.water_target}
        values += [(self.EXTRA_PREFIX + k, v, False) for k, v in extra.items()]
        return values

    def on_write(self, idx, field, value, read):
        if idx == OUT_ROOM and field == 'State':
            return 'SET mode=%s' % (self.last_mode if value else 'off')
        if idx == OUT_ROOM and field == 'Dimming':
            sp = int(round(min(max(float(value), ROOM_MIN), ROOM_MAX)))
            return 'SET sp=%d' % sp
        if field != 'Dimming':
            return None
        if idx == OUT_MODE:
            return 'SET mode=%s' % _pick(MODES, value)
        if idx == OUT_WATER:
            return 'SET ww=%d' % _pick(WATER_VALUES, value)
        if idx == OUT_MIX:
            return 'SET mix=%s' % _pick(MIXES, value)
        if idx == OUT_ELECTRIC:
            return 'SET el=%d' % _pick(EL_VALUES, value)
        return None
