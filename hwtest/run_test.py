#!/usr/bin/env python3
"""Testet die LIN-Ports zweier ESP32_LIN-Platinen gegeneinander.

Aufbau: Beide Platinen an USB und an 12 V. Eine gemeinsame LIN-Leitung verbindet
J2 Pin 1 und J5 Pin 1 beider Platinen, dazu eine GND-Leitung. Pro Phase steckt
JP1 nur auf der Platine, deren LIN2 gerade Master ist.

    python run_test.py --phase 1     # JP1 auf Platine A, nicht auf B
    python run_test.py --phase 2     # JP1 auf Platine B, nicht auf A

A und B sind die beiden Platinen, sortiert nach MAC-Adresse.
Benötigt pyserial (z.B. ~/.platformio/penv/bin/python).
"""
import argparse
import glob
import re
import sys
import time

import serial

BAUDS = (9600, 19200)


class Board:
    def __init__(self, port):
        self.port = port
        self.s = serial.Serial(port, 115200, timeout=0.1)
        time.sleep(1.5)
        self.s.reset_input_buffer()
        self.mac = self.ask("ID", "ID ", 3).split()[1]

    def ask(self, cmd, prefix, timeout):
        self.s.write((cmd + "\n").encode())
        end = time.time() + timeout
        nudge = time.time() + 2
        while time.time() < end:
            # USB-Ausgaben können im Puffer hängen, bis die Platine wieder etwas sendet.
            if prefix != "ID " and time.time() > nudge:
                self.s.write(b"ID\n")
                nudge = time.time() + 2
            line = self.s.readline().decode("utf8", "replace").strip()
            if line.startswith(prefix) or line.startswith("ERR"):
                return line
        raise RuntimeError(f"{self.mac if hasattr(self, 'mac') else self.port}: keine Antwort auf {cmd}")


def fields(line):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}


def run(master, slave, slave_port, baud, n):
    label = f"{master.name}.LIN2 (Master) <-> {slave.name}.LIN{slave_port} (Slave), {baud} Baud"
    slave.ask(f"SLAVE {slave_port} {baud}", "OK", 3)
    r = fields(master.ask(f"MASTER 2 {baud} {n}", "RESULT", n * 0.2 + 5))  # reicht auch, wenn jeder Frame in Timeouts läuft
    s = fields(slave.ask("IDLE", "STATS", 3))
    ok = (
        r.get("echoErr") == 0
        and r.get("respOk") == n
        and s.get("rxOk") == n
        and s.get("txEchoErr") == 0
        and s.get("tx") == n
    )
    print(f"{'OK    ' if ok else 'FEHLER'} {label}")
    print(f"       Master: {r}")
    print(f"       Slave:  {s}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--phase", type=int, choices=(1, 2), required=True)
    ap.add_argument("-n", type=int, default=200, help="Frames je Richtung und Test")
    args = ap.parse_args()

    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(ports) != 2:
        sys.exit(f"Genau zwei Platinen an USB erwartet, gefunden: {ports}")
    boards = sorted((Board(p) for p in ports), key=lambda b: b.mac)
    for name, b in zip("AB", boards):
        b.name = name
        print(f"Platine {name}: MAC {b.mac} an {b.port}")

    master, slave = boards if args.phase == 1 else reversed(boards)
    print(f"Phase {args.phase}: JP1 muss auf {master.name} stecken, nicht auf {slave.name}\n")
    for b in boards:
        b.ask("IDLE", "STATS", 3)

    results = [run(master, slave, port, baud, args.n) for port in (1, 2) for baud in BAUDS]
    print(f"\n{sum(results)}/{len(results)} Tests bestanden")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
