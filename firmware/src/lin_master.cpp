#include "lin_master.h"

void LinMaster::begin() {
  if (pins_.slp >= 0) {  // TJA1021: SLP high = Normal Mode
    pinMode(pins_.slp, OUTPUT);
    digitalWrite(pins_.slp, HIGH);
  }
  ser_.begin(BAUD, SERIAL_8N1, pins_.rx, pins_.tx);
  ser_.setTimeout(0);
}

// Break: 0x00 bei 9600 Baud hält die Leitung 9 Bit × 104 µs ≈ 0,94 ms
// dominant – das sind 18 Bitzeiten bei 19200 Baud (Spezifikation: ≥ 13).
void LinMaster::sendBreak() {
  ser_.flush();
  ser_.updateBaudRate(BAUD / 2);
  ser_.write(uint8_t(0x00));
  ser_.flush();
  ser_.updateBaudRate(BAUD);
}

void LinMaster::sendFrame(uint8_t id, const uint8_t data[8]) {
  const uint8_t p = alde::pid(id);
  uint8_t frame[11] = {0x55, p};
  memcpy(frame + 2, data, 8);
  frame[10] = alde::checksumEnhanced(p, data, 8);

  while (ser_.available()) ser_.read();
  sendBreak();
  ser_.write(frame, sizeof frame);
  ser_.flush();
#ifdef HEATER_SIM
  if (id == alde::ID_CONTROL) {
    simReadControlEcho();
    return;
  }
#endif
  drain(3);  // eigenes Echo verwerfen
}

void LinMaster::drain(uint32_t ms) {
  uint32_t until = millis() + ms;
  while ((int32_t)(millis() - until) < 0) {
    while (ser_.available()) ser_.read();
    delay(1);
  }
}

uint32_t LinMaster::discardInput() {
  uint32_t n = 0;
  while (ser_.available()) {
    ser_.read();
    n++;
  }
  return n;
}

void LinMaster::sendDiag() { sendFrame(alde::ID_DIAG, alde::DIAG_PAYLOAD); }

void LinMaster::sendControl(const uint8_t data[8]) { sendFrame(alde::ID_CONTROL, data); }

bool LinMaster::readInfo(uint8_t data[8]) {
  while (ser_.available()) ser_.read();
  sendBreak();
  const uint8_t header[2] = {0x55, alde::pid(alde::ID_INFO)};
  ser_.write(header, sizeof header);
  ser_.flush();
#ifdef HEATER_SIM
  simAnswerInfo();
#endif

  // Echo (Break-Rest, 0x55, PID) + 8 Daten + Checksumme; 9 Bytes bei 19200
  // Baud dauern ≈ 4,7 ms. Fenster großzügig, Ende sobald gültig.
  uint8_t buf[32];
  size_t len = 0;
  uint32_t start = millis();
  while (millis() - start < 40 && len < sizeof buf) {
    while (ser_.available() && len < sizeof buf) buf[len++] = ser_.read();
    if (alde::findInfoResponse(buf, len, data)) return true;
    delay(1);
  }
  return false;
}

#ifdef HEATER_SIM
// Antwort des simulierten Panels direkt hinter den Header auf den Bus legen.
// Sie läuft durch den Transceiver zurück und wird von readInfo() wie eine
// echte Antwort aus dem Echo gelesen. Ohne 12 V am Transceiver kommt nichts
// zurück und es bleibt bei link=0.
void LinMaster::simAnswerInfo() {
  const uint32_t now = millis();
  if (simLastStep_) sim_.step((now - simLastStep_) / 1000.0f);
  simLastStep_ = now;

  uint8_t resp[9];
  sim_.info(resp);
  resp[8] = alde::checksumEnhanced(alde::pid(alde::ID_INFO), resp, 8);
  ser_.write(resp, sizeof resp);
}

// Steuer-Frame aus dem eigenen Echo lesen, so wie ihn das Panel empfangen würde.
void LinMaster::simReadControlEcho() {
  uint8_t buf[32], data[8];
  size_t len = 0;
  const uint32_t start = millis();
  while (millis() - start < 10 && len < sizeof buf) {
    while (ser_.available() && len < sizeof buf) buf[len++] = ser_.read();
    if (alde::findFrame(buf, len, alde::pid(alde::ID_CONTROL), data)) {
      sim_.control(data);
      return;
    }
    delay(1);
  }
}
#endif
