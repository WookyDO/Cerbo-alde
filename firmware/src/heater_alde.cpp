#include "heater_alde.h"

#include "config.h"
#include "io.h"

static constexpr uint32_t GUARD_LISTEN_MS = 1500;   // Bus abhören vor dem ersten Senden
static constexpr uint32_t GUARD_RETRY_MS = 10000;   // erneut prüfen nach fremdem Master

AldeHeater::AldeHeater() : lin_(Serial1, LIN_ALDE) {}

void AldeHeater::begin() {
  lin_.begin();
  lin_.discardInput();
  guardStart_ = millis();
  guardBytes_ = 0;
}

// Die Alde-Bedienteile senden nie von sich aus. Kommen trotzdem Daten, hängt
// ein anderer Master am Bus (z.B. ein Truma CP Plus) – dann nicht senden.
void AldeHeater::guard() {
  guardBytes_ += lin_.discardInput();
  if (millis() - guardStart_ < GUARD_LISTEN_MS) return;
  if (guardBytes_ == 0) {
    guarded_ = true;
    lastError_[0] = '\0';
    lin_.sendDiag();
    lastDiag_ = millis();
    return;
  }
  if (!lastError_[0]) {
    snprintf(lastError_, sizeof lastError_,
             "Fremder LIN-Master am Bus (Truma?) - Heizungstyp pruefen");
    logf("%s, sende nicht", lastError_);
    char line[96];
    snprintf(line, sizeof line, "ERR %s", lastError_);
    emitLine(line);
  }
  publish(false);
  if (millis() - guardStart_ >= GUARD_RETRY_MS) {
    guardStart_ = millis();
    guardBytes_ = 0;
  }
}

void AldeHeater::loop() {
  if (!guarded_) {
    guard();
    return;
  }
  const uint32_t interval = pending_.any() ? CONFIRM_INTERVAL_MS : POLL_INTERVAL_MS;
  if (lastPoll_ == 0 || millis() - lastPoll_ >= interval) {
    lastPoll_ = millis();
    cycle();
  }
}

void AldeHeater::publish(bool force) {
  char line[200];
  alde::formatState(linkUp_ ? &current_ : nullptr, line, sizeof line);
  publishLine(line, force);
}

size_t AldeHeater::stateJson(char *out, size_t len) const {
  return alde::formatJson(linkUp_ ? &current_ : nullptr, out, len);
}

void AldeHeater::fail(const char *reason) {
  char line[96];
  snprintf(line, sizeof line, "ERR %s", reason);
  emitLine(line);
  snprintf(lastError_, sizeof lastError_, "%s", reason);
  pending_.clear();
}

bool AldeHeater::submit(const char *args, char *err, size_t errLen) {
  alde::Command cmd;
  if (!alde::parseSetArgs(args, cmd, err, errLen)) return false;
  if (!linkUp_) {
    snprintf(err, errLen, "keine Verbindung zum Panel");
    return false;
  }
  pending_.merge(cmd);
  attempts_ = 0;
  confirmPolls_ = 0;
  lastPoll_ = 0;  // sofort abarbeiten
  lastError_[0] = '\0';
  return true;
}

bool AldeHeater::command(const char *line) {
  if (strcmp(line, "DIAG") != 0) return false;
  if (guarded_) lin_.sendDiag();
  emitLine(guarded_ ? "OK" : "ERR Bus nicht frei");
  return true;
}

void AldeHeater::handlePending() {
  if (alde::commandSatisfied(current_, pending_)) {
    logf("Befehl vom Panel übernommen");
    pending_.clear();
    return;
  }
  if (confirmPolls_ > 0 && confirmPolls_ < COMMAND_CONFIRM_POLLS) {
    confirmPolls_++;
    return;
  }
  if (attempts_ >= COMMAND_RETRIES) {
    fail("Panel hat Befehl nicht uebernommen");
    return;
  }
  uint8_t frame[8];
  alde::buildControl(current_, pending_, frame);
  lin_.sendControl(frame);
  attempts_++;
  confirmPolls_ = 1;
  logf("TX Control %02X %02X %02X (Versuch %u)", frame[3], frame[4], frame[5], attempts_);
}

void AldeHeater::cycle() {
  uint8_t data[8];
  if (lin_.readInfo(data)) {
    alde::decodeInfo(data, current_);
    if (!linkUp_) logf("Panel antwortet");
    linkUp_ = true;
    failCount_ = 0;
    setLinLed(true);
    if (pending_.any()) handlePending();
    publish(false);
    return;
  }

  setLinLed(false);
  if (failCount_ < 255) failCount_++;
  if (linkUp_ && failCount_ >= LINK_LOST_AFTER) {
    linkUp_ = false;
    logf("Panel antwortet nicht mehr");
    if (pending_.any()) fail("Verbindung zum Panel verloren");
  }
  if (!linkUp_) {
    // Nach Neustart oder Spannungsausfall des Panels erneut anmelden.
    if (millis() - lastDiag_ >= 5000) {
      lin_.sendDiag();
      lastDiag_ = millis();
    }
    publish(false);
  }
}
