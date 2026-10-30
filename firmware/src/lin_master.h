// LIN-Master auf dem gelben CI-Bus (TJA1021T am UART1 des WomoLIN-Controllers).
#pragma once

#include <Arduino.h>

#include "alde_protocol.h"

class LinMaster {
 public:
  LinMaster(HardwareSerial &serial, int rxPin, int txPin)
      : ser_(serial), rxPin_(rxPin), txPin_(txPin) {}

  void begin();
  // Anmeldung beim Panel (Diagnoseframe 0x3C).
  void sendDiag();
  // Info-Frame abfragen; true bei gültiger Antwort.
  bool readInfo(uint8_t data[8]);
  void sendControl(const uint8_t data[8]);

 private:
  static constexpr uint32_t BAUD = 19200;
  void sendBreak();
  void sendFrame(uint8_t id, const uint8_t data[8]);
  void drain(uint32_t ms);

  HardwareSerial &ser_;
  int rxPin_, txPin_;
};
