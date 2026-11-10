# ESP32_LIN – vorgemerkte Änderungen für Rev 5

Stand: 01.11.2026. Rev 4 ist bestellt. Diese Punkte sind **noch nicht umgesetzt**.

## 1. Bohrung der Schraubklemmen J1, J2, J5 auf 1,5 mm vergrößern

- Heute: Footprint Phoenix MKDS 1,5 (5,08 mm), Bohrung 1,3 mm, Pad 2,6 mm.
- Problem: Same Sky TB007-508 (DigiKey 102-6203-ND / 102-6204-ND) hat Stifte 1,0 × 0,8 mm (Diagonale 1,28 mm), Same Sky empfiehlt 1,6 mm, 1,5 mm lässt immer noch gut 0,2 mm Spiel. In 1,3 mm passt der Stift nur ohne jeden Spielraum.
- Änderung: Bohrung 1,5 mm, Pad 2,6 mm (Restring 0,55 mm). Passt dann für Phoenix MKDS 1,5 (Stift 0,9 × 0,9), Same Sky TB007-508 und Kefa KF128.
- Kontrollieren: Abstand Pad–Pad und Pad–Leiterbahn (DRC), JLCDFM-Prüfung.
- Hinweis: Die TB007 ist 10,25 mm tief (MKDS 9,8 mm) und steht auf der Kabelseite 0,75 mm weiter vor. Ihre Vorderkante liegt dann etwa 0,25 mm innerhalb der Platinenkante. Optional die Klemmen 0,5 mm nach innen schieben.

## Für Rev 4 (bestellt)

Bohrung 1,3 mm → Phoenix-Originale verwenden: 1715721 (277-1263-ND) für J1, 1715734 (277-1264-ND) für J2/J5.
