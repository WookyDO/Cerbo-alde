"""Heizungsmodelle: Zeilenprotokoll der Bridge und Abbildung auf Venus-OS-Pfade.

Ohne D-Bus-Abhängigkeiten, damit sie auf dem PC testbar sind. Jedes Modell
beschreibt seine Schaltausgänge und Temperaturdienste und übersetzt in beide
Richtungen zwischen STATE/SET-Zeilen und D-Bus-Werten.
"""

from . import alde, truma

MODELS = {alde.Model.TYPE: alde.Model, truma.Model.TYPE: truma.Model}


def parse_hello(rest):
    """'lin-bridge 1.1 heater=truma' -> 'truma'; ältere Firmware kennt nur die Alde."""
    for tok in rest.split():
        if tok.startswith('heater='):
            return tok.split('=', 1)[1]
    return alde.Model.TYPE
