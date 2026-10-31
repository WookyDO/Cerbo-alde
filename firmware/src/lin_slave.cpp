#include "lin_slave.h"

using namespace truma;

static constexpr uint32_t BYTE_TIMEOUT_MS = 5;  // innerhalb eines Frames (1 Byte ≈ 1 ms)

void LinSlave::begin(InetBox *box, SemaphoreHandle_t lock) {
  box_ = box;
  lock_ = lock;
  ser_.begin(BAUD, SERIAL_8N1, rxPin_, txPin_);
  // Bytes schon nach einer Zeichenzeit Pause weiterreichen, sonst kommt der
  // Header erst verspätet an und die Antwort verpasst ihr Zeitfenster.
  ser_.setRxTimeout(1);
  xTaskCreatePinnedToCore(taskEntry, "lin-slave", 4096, this, 5, nullptr, 1);
}

void LinSlave::taskEntry(void *self) { static_cast<LinSlave *>(self)->run(); }

int LinSlave::readByte(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (!ser_.available()) {
    if (millis() - start >= timeoutMs) return -1;
    vTaskDelay(1);
  }
  lastByteAt_ = millis();
  return ser_.read();
}

size_t LinSlave::readFrame(uint8_t *buf, size_t max) {
  size_t n = 0;
  while (n < max) {
    const int c = readByte(BYTE_TIMEOUT_MS);
    if (c < 0) break;
    buf[n++] = c;
  }
  return n;
}

// Header = Break (liest sich als 0x00) + Sync 0x55 + PID. Fehlt das 0x00,
// zählt eine 0x55 nach einer Pause als Sync.
void LinSlave::run() {
  int prev = -1;
  for (;;) {
    const uint32_t idleSince = lastByteAt_;
    const int c = readByte(1000);
    if (c < 0) {
      prev = -1;
      continue;
    }
    const bool gap = lastByteAt_ - idleSince >= 3;
    if (c == 0x55 && (prev == 0x00 || gap)) {
      const int p = readByte(BYTE_TIMEOUT_MS);
      prev = -1;
      if (p >= 0 && pidParityOk(p)) handleHeader(p & 0x3F);
      continue;
    }
    prev = c;
  }
}

void LinSlave::handleHeader(uint8_t id) {
  headers_++;
  uint8_t d[9];
  if (id == ID_UPDATE) {
    lastUpdate_ = millis();
    xSemaphoreTake(lock_, portMAX_DELAY);
    box_->updateResponse(d);
    xSemaphoreGive(lock_);
    send(id, d);
  } else if (id == ID_SLAVE_RESP) {
    xSemaphoreTake(lock_, portMAX_DELAY);
    const bool has = box_->nextResponse(d);
    xSemaphoreGive(lock_);
    if (has)
      send(id, d);
    else if (log_)
      logFrame("RX", id, d, readFrame(d, 9));
    else
      readFrame(d, 9);  // Antwort eines anderen Slaves überspringen
  } else if (id == ID_MASTER_REQ) {
    const size_t n = readFrame(d, 9);
    if (log_) logFrame("RX", id, d, n);
    if (n == 9 && d[8] == frameChecksum(id, d)) {
      xSemaphoreTake(lock_, portMAX_DELAY);
      box_->onMasterRequest(d);
      xSemaphoreGive(lock_);
    }
  } else {
    const size_t n = readFrame(d, 9);
    if (log_) logFrame("RX", id, d, n);
  }
}

void LinSlave::send(uint8_t id, const uint8_t data[8]) {
  uint8_t buf[9];
  memcpy(buf, data, 8);
  buf[8] = frameChecksum(id, data);
  ser_.write(buf, sizeof buf);
  ser_.flush();
  // Der Transceiver liefert die eigenen Bytes als Echo zurück.
  uint8_t echo[9];
  readFrame(echo, sizeof echo);
  if (log_) logFrame("TX", id, buf, sizeof buf);
}

void LinSlave::logFrame(const char *dir, uint8_t id, const uint8_t *d, size_t n) {
  char line[72];
  int len = snprintf(line, sizeof line, "LIN %s %02X:", dir, id);
  for (size_t i = 0; i < n && len < (int)sizeof line - 3; i++)
    len += snprintf(line + len, sizeof line - len, " %02X", d[i]);
  portENTER_CRITICAL(&logMux_);
  strcpy(logBuf_[(logHead_ + logCount_) % LOG_LINES], line);
  if (logCount_ < LOG_LINES)
    logCount_++;
  else
    logHead_ = (logHead_ + 1) % LOG_LINES;  // ältesten überschreiben
  portEXIT_CRITICAL(&logMux_);
}

bool LinSlave::popLog(char *out, size_t len) {
  bool ok = false;
  portENTER_CRITICAL(&logMux_);
  if (logCount_) {
    strncpy(out, logBuf_[logHead_], len - 1);
    out[len - 1] = '\0';
    logHead_ = (logHead_ + 1) % LOG_LINES;
    logCount_--;
    ok = true;
  }
  portEXIT_CRITICAL(&logMux_);
  return ok;
}
