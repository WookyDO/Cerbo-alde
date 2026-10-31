// Alde Compact 3030 Plus: die Bridge ist LIN-Master am gelben CI-Bus.
#pragma once

#include "alde_protocol.h"
#include "heater.h"
#include "lin_master.h"

class AldeHeater : public Heater {
 public:
  AldeHeater();
  const char *type() const override { return "alde"; }
  void begin() override;
  void loop() override;
  bool linkUp() const override { return linkUp_; }
  void publish(bool force) override;
  size_t stateJson(char *out, size_t len) const override;
  bool submit(const char *args, char *err, size_t errLen) override;
  bool pending() const override { return pending_.any(); }
  const char *lastError() const override { return lastError_; }
  bool command(const char *line) override;

 private:
  void guard();
  void cycle();
  void handlePending();
  void fail(const char *reason);

  LinMaster lin_;
  alde::State current_;
  bool linkUp_ = false;
  uint8_t failCount_ = 0;
  uint32_t lastPoll_ = 0, lastDiag_ = 0;
  alde::Command pending_;
  uint8_t attempts_ = 0, confirmPolls_ = 0;
  char lastError_[80] = "";

  // Vor dem ersten Senden prüfen, ob schon ein Master (z.B. ein CP Plus) sendet.
  bool guarded_ = false;
  uint32_t guardStart_ = 0, guardBytes_ = 0;
};
