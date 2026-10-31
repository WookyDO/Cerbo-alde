// LIN-Slave für das Truma CP Plus (TJA1021T am UART1). Läuft als eigene
// FreeRTOS-Task, damit Webserver und WLAN die Antwortzeit nicht verzögern.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "truma_protocol.h"

class LinSlave {
 public:
  LinSlave(HardwareSerial &serial, int rxPin, int txPin)
      : ser_(serial), rxPin_(rxPin), txPin_(txPin) {}

  // Startet die Task; Zugriffe auf box laufen unter lock.
  void begin(truma::InetBox *box, SemaphoreHandle_t lock);
  // Zeitpunkt (millis) des letzten 0x18-Headers vom CP Plus, 0 = noch keiner.
  uint32_t lastUpdateHeader() const { return lastUpdate_; }
  uint32_t headerCount() const { return headers_; }

  // Mitschnitt aller Frames als Logzeilen ("LINLOG 1").
  void setLog(bool on) { log_ = on; }
  bool popLog(char *out, size_t len);

 private:
  static void taskEntry(void *self);
  void run();
  int readByte(uint32_t timeoutMs);
  size_t readFrame(uint8_t *buf, size_t max);
  void handleHeader(uint8_t id);
  void send(uint8_t id, const uint8_t data[8]);
  void logFrame(const char *dir, uint8_t id, const uint8_t *d, size_t n);

  HardwareSerial &ser_;
  int rxPin_, txPin_;
  truma::InetBox *box_ = nullptr;
  SemaphoreHandle_t lock_ = nullptr;
  volatile uint32_t lastUpdate_ = 0, headers_ = 0;
  volatile bool log_ = false;
  uint32_t lastByteAt_ = 0;

  // Ringpuffer für Logzeilen (Task schreibt, loop() liest)
  static constexpr int LOG_LINES = 16;
  char logBuf_[LOG_LINES][72];
  int logHead_ = 0, logCount_ = 0;
  portMUX_TYPE logMux_ = portMUX_INITIALIZER_UNLOCKED;
};
