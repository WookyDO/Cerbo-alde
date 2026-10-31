#include "alde_sim.h"

#include <cmath>

#include "alde_protocol.h"

namespace alde {

static constexpr float HEAT_RATE = 0.05f;   // °C/s beim Heizen (für Tests beschleunigt)
static constexpr float COOL_FACTOR = 0.002f; // Anteil der Differenz nach außen pro s

static uint8_t encodeTemp(float celsius) {
  long raw = std::lround((celsius + 42.0f) * 2.0f);
  if (raw < 0) raw = 0;
  if (raw > 0xFA) raw = 0xFA;
  return static_cast<uint8_t>(raw);
}

static constexpr float HYSTERESIS = 0.5f;   // °C unter Sollwert bis zum Wiedereinschalten

// Zweipunktregler: heizt bis zum Sollwert, startet erst wieder 0,5 °C darunter.
void PanelSim::updateHeating() {
  const float sp = decodeSetpoint(setpoint_);
  if (!gas_ && electric_ == 0) heating_ = false;
  else if (zone1_ >= sp) heating_ = false;
  else if (zone1_ <= sp - HYSTERESIS) heating_ = true;
}

void PanelSim::info(uint8_t out[8]) const {
  const bool heat = heating_;
  out[0] = encodeTemp(zone1_);
  out[1] = 0xFE;  // Zone 2 nicht vorhanden
  out[2] = encodeTemp(outdoor_);
  out[3] = setpoint_ | (gas_ ? 0x40 : 0) | ((gas_ && (heat || water_)) ? 0x80 : 0);
  out[4] = 0x1E | (electric_ << 6);
  // Panel an, Warmwasser, 230 V vorhanden, Pumpe beim Heizen
  out[5] = 0x01 | (water_ << 3) | 0x20 | (heat ? 0x80 : 0);
  out[6] = 0x00;
  out[7] = 0x00;
}

void PanelSim::control(const uint8_t d[8]) {
  setpoint_ = d[3] & 0x3F;
  if (setpoint_ > encodeSetpoint(SETPOINT_MAX)) setpoint_ = encodeSetpoint(SETPOINT_MAX);
  gas_ = (d[3] >> 6) & 1;
  electric_ = (d[4] >> 6) & 3;
  water_ = (d[5] >> 3) & 3;
  if (water_ == 3) water_ = WATER_ON;
  updateHeating();
}

void PanelSim::step(float seconds) {
  if (heating_) {
    zone1_ += HEAT_RATE * seconds;
    float sp = decodeSetpoint(setpoint_);
    if (zone1_ > sp) zone1_ = sp;
  } else {
    zone1_ -= (zone1_ - outdoor_) * COOL_FACTOR * seconds;
  }
  updateHeating();
}

}  // namespace alde
