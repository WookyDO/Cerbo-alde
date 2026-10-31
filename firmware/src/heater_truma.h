// Truma Combi mit CP Plus: die Bridge gibt sich als iNet Box aus (LIN-Slave).
// Befehle holt das CP Plus selbst ab; bis zur Bestätigung können ~30 s vergehen.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "heater.h"
#include "truma_protocol.h"
#ifndef HEATER_SIM
#include "lin_slave.h"
#endif

class TrumaHeater : public Heater {
 public:
  TrumaHeater();
  const char *type() const override { return "truma"; }
  void begin() override;
  void loop() override;
  bool linkUp() const override { return linkUp_; }
  void publish(bool force) override;
  size_t stateJson(char *out, size_t len) const override;
  bool submit(const char *args, char *err, size_t errLen) override;
  bool pending() const override { return sent_.any(); }
  const char *lastError() const override { return lastError_; }
  bool command(const char *line) override;

 private:
  void fail(const char *reason);
  uint32_t lastUpdateHeader() const;

  truma::InetBox box_;
  SemaphoreHandle_t lock_;
#ifdef HEATER_SIM
  truma::CpPlusSim sim_;
  uint32_t lastSim_ = 0, simUpdate_ = 0;
#else
  LinSlave slave_;
#endif

  truma::State current_;
  bool linkUp_ = false;
  uint32_t seenStatus_ = 0;
  float lastRoomTarget_ = 20;
  truma::Command sent_;  // eingereiht, wartet auf Bestätigung im Status
  uint32_t sentAt_ = 0;
  uint8_t attempts_ = 0;
  uint32_t startedAt_ = 0;
  bool noPanelHint_ = false;
  char lastError_[80] = "";
};
