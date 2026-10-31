// Ausgabe an USB und TCP-Client, gemeinsam für main und die Heizungen.
#pragma once

// Protokollzeile (STATE, OK, ERR ...) an USB und TCP.
void emitLine(const char *line);
// Logzeile "# ..." an USB und TCP; der Cerbo-Dienst ignoriert sie.
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// LEDs des WomoLIN-Controllers sind aktiv low.
void setLed(int pin, bool on);
