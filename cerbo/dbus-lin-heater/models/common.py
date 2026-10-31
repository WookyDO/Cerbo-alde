"""Gemeinsame Konstanten und Hilfen der Heizungsmodelle."""

from dataclasses import dataclass, field

# gui-v2 enums.h: SwitchableOutput_Type / _Status, Temperature_DeviceType
TYPE_TOGGLE, TYPE_SETPOINT, TYPE_STEPPED, TYPE_DROPDOWN = 1, 3, 4, 6
STATUS_OFF, STATUS_ON = 0x00, 0x09
TEMP_ROOM, TEMP_OUTDOOR, TEMP_WATER_HEATER = 3, 4, 5


@dataclass
class Output:
    """Ein Schaltausgang /SwitchableOutput/<idx> im Switch-Dienst."""
    idx: int
    name_key: str
    type: int
    state_writeable: bool = False
    dimming: object = None
    measurement: bool = False
    settings: dict = field(default_factory=dict)


@dataclass
class Temperature:
    """Ein eigener com.victronenergy.temperature-Dienst."""
    key: str           # Schlüssel in temp_values()
    service: str       # Suffix des D-Bus-Namens
    instance_key: str  # Option in [dbus]
    name_key: str      # Option in [names.<typ>]
    temp_type: int


def split_state(args):
    """'link=1 a=1 b=-' -> dict oder None, wenn link nicht 1 ist."""
    kv = dict(tok.split('=', 1) for tok in args.split() if '=' in tok)
    return kv if kv.get('link') == '1' else None


def temp(kv, key):
    v = kv.get(key, '-')
    return None if v == '-' else float(v)
