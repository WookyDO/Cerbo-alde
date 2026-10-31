#include <unity.h>

#include <cmath>
#include <cstring>

#include "alde_protocol.h"
#include "alde_sim.h"

using namespace alde;

void setUp() {}
void tearDown() {}

static State stateOf(const PanelSim &sim) {
  uint8_t d[8];
  sim.info(d);
  State s;
  decodeInfo(d, s);
  return s;
}

static void apply(PanelSim &sim, const char *args) {
  Command c;
  char err[48];
  TEST_ASSERT_TRUE(parseSetArgs(args, c, err, sizeof err));
  uint8_t frame[8];
  buildControl(stateOf(sim), c, frame);
  sim.control(frame);
}

void test_initial_state() {
  State s = stateOf(PanelSim());
  TEST_ASSERT_EQUAL_FLOAT(18.0f, s.zone1);
  TEST_ASSERT_TRUE(std::isnan(s.zone2));
  TEST_ASSERT_EQUAL_FLOAT(8.0f, s.outdoor);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, s.setpoint);
  TEST_ASSERT_FALSE(s.gas);
  TEST_ASSERT_TRUE(s.panelOn);
  TEST_ASSERT_FALSE(s.pump);
}

void test_info_frame_found_with_checksum() {
  PanelSim sim;
  uint8_t buf[12] = {0x55, 0x5B};
  sim.info(buf + 2);
  buf[10] = checksumEnhanced(0x5B, buf + 2, 8);
  uint8_t out[8];
  TEST_ASSERT_TRUE(findInfoResponse(buf, 11, out));
}

void test_control_roundtrip_satisfies_command() {
  PanelSim sim;
  Command c;
  char err[48];
  TEST_ASSERT_TRUE(parseSetArgs("sp=22.5 gas=1 el=2 ww=2", c, err, sizeof err));
  uint8_t frame[8];
  buildControl(stateOf(sim), c, frame);

  // Steuer-Frame wie auf dem Bus: 0x55, PID 0x1A, Daten, Checksumme
  uint8_t bus[12] = {0x00, 0x55, 0x1A};
  memcpy(bus + 3, frame, 8);
  bus[11] = checksumEnhanced(0x1A, frame, 8);
  uint8_t rx[8];
  TEST_ASSERT_TRUE(findFrame(bus, sizeof bus, pid(ID_CONTROL), rx));
  sim.control(rx);

  State s = stateOf(sim);
  TEST_ASSERT_TRUE(commandSatisfied(s, c));
  TEST_ASSERT_TRUE(s.valve);
  TEST_ASSERT_TRUE(s.pump);
}

void test_heats_to_setpoint_and_cools_when_off() {
  PanelSim sim;
  apply(sim, "sp=21 gas=1");
  for (int i = 0; i < 200; i++) sim.step(1.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 21.0f, sim.zone1());  // regelt um den Sollwert

  apply(sim, "gas=0");
  for (int i = 0; i < 100; i++) sim.step(1.0f);
  TEST_ASSERT_TRUE(sim.zone1() < 21.0f);
  TEST_ASSERT_TRUE(sim.zone1() > 8.0f);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_initial_state);
  RUN_TEST(test_info_frame_found_with_checksum);
  RUN_TEST(test_control_roundtrip_satisfies_command);
  RUN_TEST(test_heats_to_setpoint_and_cools_when_off);
  return UNITY_END();
}
