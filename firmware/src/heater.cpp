#include "heater.h"

#include "config.h"
#include "heater_alde.h"
#include "heater_truma.h"
#include "io.h"

void Heater::publishLine(const char *line, bool force) {
  if (force || strcmp(line, lastLine_) != 0 || millis() - lastPublish_ >= STATE_HEARTBEAT_MS) {
    emitLine(line);
    strlcpy(lastLine_, line, sizeof lastLine_);
    lastPublish_ = millis();
  }
}

Heater *createHeater(const String &type) {
  if (type == "truma") return new TrumaHeater();
  return new AldeHeater();
}
