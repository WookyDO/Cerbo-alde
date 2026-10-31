// Gemeinsame Schnittstelle der unterstützten Heizungen. Welche läuft, steht in
// den Einstellungen (Setup-Seite); ein Wechsel braucht einen Neustart, weil die
// Bridge an der Alde LIN-Master und an der Truma LIN-Slave ist.
#pragma once

#include <Arduino.h>

class Heater {
 public:
  virtual ~Heater() = default;

  virtual const char *type() const = 0;  // "alde" oder "truma"
  virtual void begin() = 0;
  virtual void loop() = 0;
  virtual bool linkUp() const = 0;
  // STATE-Zeile ausgeben: bei Änderung, als Lebenszeichen oder mit force.
  virtual void publish(bool force) = 0;
  // Zustand als JSON-Objekt, "null" ohne Verbindung.
  virtual size_t stateJson(char *out, size_t len) const = 0;
  // "SET"-Argumente annehmen und einreihen; false + err bei Fehler.
  virtual bool submit(const char *args, char *err, size_t errLen) = 0;
  virtual bool pending() const = 0;
  virtual const char *lastError() const = 0;
  // Typspezifische Befehle (z.B. DIAG, LINLOG); true, wenn behandelt.
  virtual bool command(const char *line) { return false; }

 protected:
  void publishLine(const char *line, bool force);

 private:
  char lastLine_[200] = "";
  uint32_t lastPublish_ = 0;
};

// Liefert die Heizung zum Typ aus den Einstellungen (unbekannt: Alde).
Heater *createHeater(const String &type);
