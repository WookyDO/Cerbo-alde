#include "heater_truma.h"

#include "config.h"
#include "io.h"

// Das CP Plus pausiert laut inetbox2mqtt bis zu 25 s zwischen Abfragen.
static constexpr uint32_t LINK_TIMEOUT_MS = 60000;
static constexpr uint32_t CONFIRM_TIMEOUT_MS = 60000;  // je Sendeversuch
static constexpr uint8_t TRUMA_RETRIES = 2;
static constexpr uint32_t NO_PANEL_HINT_MS = 60000;
#ifdef HEATER_SIM
static constexpr uint32_t SIM_CYCLE_MS = 1000;
#endif

TrumaHeater::TrumaHeater()
#ifndef HEATER_SIM
    : slave_(Serial1, LIN_TRUMA)
#endif
{
  lock_ = xSemaphoreCreateMutex();
}

void TrumaHeater::begin() {
  startedAt_ = millis();
#ifdef HEATER_SIM
  logf("TESTBETRIEB: CP Plus wird in Software simuliert (ohne LIN-Bus)");
#else
  slave_.begin(&box_, lock_);
  logf("Truma: warte auf CP Plus (iNet Box, NAD 0x03)");
#endif
}

uint32_t TrumaHeater::lastUpdateHeader() const {
#ifdef HEATER_SIM
  return simUpdate_;
#else
  return slave_.lastUpdateHeader();
#endif
}

void TrumaHeater::fail(const char *reason) {
  char line[96];
  snprintf(line, sizeof line, "ERR %s", reason);
  emitLine(line);
  snprintf(lastError_, sizeof lastError_, "%s", reason);
  sent_.clear();
}

void TrumaHeater::loop() {
  const uint32_t now = millis();
#ifdef HEATER_SIM
  if (now - lastSim_ >= SIM_CYCLE_MS) {
    const float dt = lastSim_ ? (now - lastSim_) / 1000.0f : 0;
    lastSim_ = now;
    xSemaphoreTake(lock_, portMAX_DELAY);
    sim_.cycle(box_, dt);
    xSemaphoreGive(lock_);
    simUpdate_ = now;
  }
#else
  char logLine[72];
  while (slave_.popLog(logLine, sizeof logLine)) logf("%s", logLine);
#endif

  xSemaphoreTake(lock_, portMAX_DELAY);
  const uint32_t count = box_.statusCount();
  const bool queued = box_.commandPending();
  if (count != seenStatus_) current_ = box_.state();
  xSemaphoreGive(lock_);

  const uint32_t header = lastUpdateHeader();
  const bool up = count > 0 && header && now - header < LINK_TIMEOUT_MS;
  if (up != linkUp_) {
    linkUp_ = up;
    logf(up ? "CP Plus verbunden" : "CP Plus antwortet nicht mehr");
    if (!up && sent_.any()) fail("Verbindung zum CP Plus verloren");
  }
  setLinLed(linkUp_);

  if (!header && !noPanelHint_ && now - startedAt_ >= NO_PANEL_HINT_MS) {
    noPanelHint_ = true;
    snprintf(lastError_, sizeof lastError_,
             "Kein CP Plus gefunden - Heizungstyp, Kabel und PR SET pruefen");
    logf("%s", lastError_);
  } else if (header && noPanelHint_) {
    noPanelHint_ = false;
    lastError_[0] = '\0';
  }

  if (count != seenStatus_) {
    seenStatus_ = count;
    if (current_.roomTarget >= truma::ROOM_MIN) lastRoomTarget_ = current_.roomTarget;
    if (sent_.any() && truma::commandSatisfied(current_, sent_)) {
      logf("Befehl vom CP Plus uebernommen");
      sent_.clear();
    }
  }

  // Wartet der Befehl nicht mehr in der iNet Box, hat ihn das CP Plus abgeholt.
  if (sent_.any() && !queued && now - sentAt_ >= CONFIRM_TIMEOUT_MS) {
    if (attempts_ >= TRUMA_RETRIES) {
      fail("CP Plus hat Befehl nicht uebernommen");
    } else {
      attempts_++;
      sentAt_ = now;
      xSemaphoreTake(lock_, portMAX_DELAY);
      box_.queue(sent_);
      xSemaphoreGive(lock_);
      logf("Befehl erneut eingereiht (Versuch %u)", attempts_ + 1);
    }
  }
  publish(false);
}

void TrumaHeater::publish(bool force) {
  char line[200];
  truma::formatState(linkUp_ ? &current_ : nullptr, line, sizeof line);
  publishLine(line, force);
}

size_t TrumaHeater::stateJson(char *out, size_t len) const {
  return truma::formatJson(linkUp_ ? &current_ : nullptr, out, len);
}

bool TrumaHeater::submit(const char *args, char *err, size_t errLen) {
  truma::Command cmd;
  if (!truma::parseSetArgs(args, cmd, err, errLen)) return false;
  if (!linkUp_) {
    snprintf(err, errLen, "keine Verbindung zum CP Plus");
    return false;
  }
  truma::normalize(current_, lastRoomTarget_, cmd);
  xSemaphoreTake(lock_, portMAX_DELAY);
  box_.queue(cmd);
  xSemaphoreGive(lock_);
  sent_.merge(cmd);
  sentAt_ = millis();
  attempts_ = 0;
  lastError_[0] = '\0';
  return true;
}

// "LINLOG 1"/"LINLOG 0": alle Frames am Bus als Logzeilen ausgeben.
bool TrumaHeater::command(const char *line) {
  if (strncmp(line, "LINLOG", 6) != 0) return false;
#ifdef HEATER_SIM
  emitLine("ERR im Testbetrieb gibt es keinen Bus");
#else
  slave_.setLog(!strcmp(line, "LINLOG 1"));
  emitLine("OK");
#endif
  return true;
}
