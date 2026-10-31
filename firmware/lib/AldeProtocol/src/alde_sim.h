// Simuliertes Alde-Panel für Tests ohne Heizung (env:womolin_sim).
//
// Liefert Info-Frames wie das echte Panel und übernimmt Steuer-Frames.
// Die Innentemperatur folgt einem einfachen Modell: Heizen Richtung
// Sollwert, sonst Abkühlen Richtung Außentemperatur.
#pragma once

#include <cstdint>

namespace alde {

class PanelSim {
 public:
  // Datenbytes des Info-Frames (Antwort auf PID 0x5B).
  void info(uint8_t out[8]) const;
  // Datenbytes eines Steuer-Frames (PID 0x1A) übernehmen.
  void control(const uint8_t data[8]);
  // Temperaturmodell um die vergangene Zeit weiterrechnen.
  void step(float seconds);

  float zone1() const { return zone1_; }
  bool heating() const { return heating_; }

 private:
  void updateHeating();

  bool heating_ = false;
  float zone1_ = 18.0f;
  float outdoor_ = 8.0f;
  uint8_t setpoint_ = 30;  // 20 °C (Rohwert, 0,5-°C-Raster ab 5 °C)
  bool gas_ = false;
  uint8_t electric_ = 0;
  uint8_t water_ = 0;
};

}  // namespace alde
