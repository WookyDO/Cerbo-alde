#include <unity.h>

#include <cmath>
#include <cstring>

#include "alde_protocol.h"

using namespace alde;

void setUp() {}
void tearDown() {}

static State stateFrom(const uint8_t d[8]) {
  State s;
  decodeInfo(d, s);
  return s;
}

void test_pid() {
  TEST_ASSERT_EQUAL_HEX8(0x1A, pid(ID_CONTROL));
  TEST_ASSERT_EQUAL_HEX8(0x5B, pid(ID_INFO));
  TEST_ASSERT_EQUAL_HEX8(0x3C, pid(ID_DIAG));
}

void test_checksum_matches_reference_algorithm() {
  // Referenz: daviddootson lin_checksum_enhanced (Übertrag addieren)
  const uint8_t d[8] = {0x00, 0x00, 0x00, 0xDE, 0x9E, 0x09, 0xFF, 0xFF};
  unsigned total = 0x1A;
  for (uint8_t b : d) total += b;
  while (total > 0xFF) total = (total & 0xFF) + (total >> 8);
  TEST_ASSERT_EQUAL_HEX8((~total) & 0xFF, checksumEnhanced(0x1A, d, 8));
}

void test_decode_temps() {
  TEST_ASSERT_EQUAL_FLOAT(21.0f, decodeTemp(126));
  TEST_ASSERT_EQUAL_FLOAT(-42.0f, decodeTemp(0));
  TEST_ASSERT_TRUE(std::isnan(decodeTemp(0xFE)));
  TEST_ASSERT_TRUE(std::isnan(decodeTemp(0xFB)));
}

void test_encode_setpoint_clamps_and_rounds() {
  TEST_ASSERT_EQUAL_UINT8(30, encodeSetpoint(20.0f));
  TEST_ASSERT_EQUAL_UINT8(0, encodeSetpoint(2.0f));
  TEST_ASSERT_EQUAL_UINT8(50, encodeSetpoint(40.0f));
  TEST_ASSERT_EQUAL_UINT8(33, encodeSetpoint(21.4f));  // -> 21.5
}

void test_decode_info_example() {
  // 21 °C innen, Zone 2 unbenutzt, 8 °C außen, Sollwert 20, Gas+Ventil, 2 kW,
  // Panel an, Warmwasser an, 230 V, Pumpe
  const uint8_t d[8] = {126, 0xFE, 100, 0xDE, 0x9E, 0xA9, 0, 0};
  State s = stateFrom(d);
  TEST_ASSERT_EQUAL_FLOAT(21.0f, s.zone1);
  TEST_ASSERT_TRUE(std::isnan(s.zone2));
  TEST_ASSERT_EQUAL_FLOAT(8.0f, s.outdoor);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, s.setpoint);
  TEST_ASSERT_TRUE(s.gas);
  TEST_ASSERT_TRUE(s.valve);
  TEST_ASSERT_EQUAL_UINT8(2, s.electric);
  TEST_ASSERT_EQUAL_UINT8(WATER_ON, s.water);
  TEST_ASSERT_TRUE(s.panelOn);
  TEST_ASSERT_TRUE(s.ac);
  TEST_ASSERT_TRUE(s.pump);
  TEST_ASSERT_FALSE(s.error);
}

void test_find_info_response_skips_echo() {
  const uint8_t d[8] = {126, 0xFE, 100, 0xDE, 0x9E, 0x09, 0, 0};
  uint8_t buf[16] = {0x00, 0x55, 0x5B};
  memcpy(buf + 3, d, 8);
  buf[11] = checksumEnhanced(0x5B, d, 8);
  uint8_t out[8];
  TEST_ASSERT_TRUE(findInfoResponse(buf, 12, out));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(d, out, 8);

  buf[11] ^= 1;  // falsche Checksumme
  TEST_ASSERT_FALSE(findInfoResponse(buf, 12, out));
  TEST_ASSERT_FALSE(findInfoResponse(buf, 3, out));  // nur Echo, keine Antwort
}

void test_build_control_protocol_example() {
  // Beispiel aus PROTOCOL.md: 00 00 00 DE 9E 09 FF FF
  const uint8_t d[8] = {126, 0xFE, 100, 0xDE, 0x9E, 0x09, 0, 0};
  State s = stateFrom(d);
  uint8_t out[8];
  buildControl(s, Command(), out);
  const uint8_t expect[8] = {0x00, 0x00, 0x00, 0xDE, 0x9E, 0x09, 0xFF, 0xFF};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, out, 8);
}

void test_build_control_changes_only_requested_fields() {
  const uint8_t d[8] = {126, 0xFE, 100, 0xDE, 0x9E, 0xA9, 0, 0};
  State s = stateFrom(d);
  Command c;
  c.hasSetpoint = true;
  c.setpoint = 22.5f;
  c.hasWater = true;
  c.water = WATER_BOOST;
  uint8_t out[8];
  buildControl(s, c, out);
  TEST_ASSERT_EQUAL_HEX8(35 | 0x40 | 0x80, out[3]);  // Gas + Ventil bleiben
  TEST_ASSERT_EQUAL_HEX8(0x9E, out[4]);              // Elektro bleibt 2 kW
  TEST_ASSERT_EQUAL_HEX8(0xB1, out[5]);              // Status bleibt, ww=2

  Command off;
  off.hasGas = true;
  off.gas = false;
  off.hasElectric = true;
  off.electric = 0;
  buildControl(s, off, out);
  TEST_ASSERT_EQUAL_HEX8(0x1E | 0x80, out[3]);
  TEST_ASSERT_EQUAL_HEX8(0x1E, out[4]);
}

void test_command_satisfied() {
  const uint8_t d[8] = {126, 0xFE, 100, 0xDE, 0x9E, 0x09, 0, 0};
  State s = stateFrom(d);
  Command c;
  c.hasSetpoint = true;
  c.setpoint = 20.0f;
  c.hasElectric = true;
  c.electric = 2;
  TEST_ASSERT_TRUE(commandSatisfied(s, c));
  c.electric = 3;
  TEST_ASSERT_FALSE(commandSatisfied(s, c));
}

void test_parse_set_args() {
  Command c;
  char err[48];
  TEST_ASSERT_TRUE(parseSetArgs("sp=21.5 gas=0 el=3 ww=2", c, err, sizeof err));
  TEST_ASSERT_TRUE(c.hasSetpoint && c.hasGas && c.hasElectric && c.hasWater);
  TEST_ASSERT_EQUAL_FLOAT(21.5f, c.setpoint);
  TEST_ASSERT_FALSE(c.gas);
  TEST_ASSERT_EQUAL_UINT8(3, c.electric);
  TEST_ASSERT_EQUAL_UINT8(2, c.water);

  TEST_ASSERT_FALSE(parseSetArgs("sp=31", c, err, sizeof err));
  TEST_ASSERT_FALSE(parseSetArgs("el=4", c, err, sizeof err));
  TEST_ASSERT_FALSE(parseSetArgs("gas=on", c, err, sizeof err));
  TEST_ASSERT_FALSE(parseSetArgs("foo=1", c, err, sizeof err));
  TEST_ASSERT_FALSE(parseSetArgs("", c, err, sizeof err));
}

void test_format_state() {
  const uint8_t d[8] = {126, 0xFE, 100, 0xDE, 0x9E, 0x09, 0, 0};
  State s = stateFrom(d);
  char line[200];
  formatState(&s, line, sizeof line);
  TEST_ASSERT_EQUAL_STRING(
      "STATE link=1 z1=21.0 z2=- out=8.0 sp=20.0 gas=1 valve=1 el=2 ww=1 "
      "on=1 busy=0 err=0 ac=0 pump=0 raw=DE9E09",
      line);
  formatState(nullptr, line, sizeof line);
  TEST_ASSERT_EQUAL_STRING("STATE link=0", line);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_pid);
  RUN_TEST(test_checksum_matches_reference_algorithm);
  RUN_TEST(test_decode_temps);
  RUN_TEST(test_encode_setpoint_clamps_and_rounds);
  RUN_TEST(test_decode_info_example);
  RUN_TEST(test_find_info_response_skips_echo);
  RUN_TEST(test_build_control_protocol_example);
  RUN_TEST(test_build_control_changes_only_requested_fields);
  RUN_TEST(test_command_satisfied);
  RUN_TEST(test_parse_set_args);
  RUN_TEST(test_format_state);
  return UNITY_END();
}
