#include <unity.h>

#include <cmath>
#include <cstring>

#include "truma_protocol.h"

using namespace truma;

void setUp() {}
void tearDown() {}

// Statuspuffer aus inetbox.py (Kommentar in inetbox.py): Heizung aus, Gas,
// Wasser 19,0 °C, Raum 25,2 °C.
static const uint8_t STATUS_EXAMPLE[38] = {
    0x00, 0x1e, 0x00, 0x00, 0x22, 0xff, 0xff, 0xff, 0x54, 0x01, 0x14, 0x33, 0x00,
    0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x68, 0x0b, 0xa6, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void test_pid() {
  TEST_ASSERT_EQUAL_HEX8(0xD8, pid(ID_UPDATE));
  TEST_ASSERT_EQUAL_HEX8(0x3C, pid(ID_MASTER_REQ));
  TEST_ASSERT_EQUAL_HEX8(0x7D, pid(ID_SLAVE_RESP));
  TEST_ASSERT_TRUE(pidParityOk(0xD8));
  TEST_ASSERT_FALSE(pidParityOk(0x18));
}

void test_frame_checksums_from_reference() {
  // Antwort auf 0x18 (mit PID 0xD8 in der Prüfsumme)
  uint8_t d[8];
  memset(d, 0xFF, 8);
  TEST_ASSERT_EQUAL_HEX8(0x27, frameChecksum(ID_UPDATE, d));
  d[0] = 0xFE;
  TEST_ASSERT_EQUAL_HEX8(0x28, frameChecksum(ID_UPDATE, d));
  // Mitschnitte aus der inetbox.py-README (klassische Prüfsumme)
  const uint8_t readId[8] = {0x7F, 0x06, 0xB2, 0x00, 0x17, 0x46, 0x00, 0x1F};
  TEST_ASSERT_EQUAL_HEX8(0x4B, frameChecksum(ID_MASTER_REQ, readId));
  const uint8_t other[8] = {0x01, 0x06, 0xB8, 0x40, 0x03, 0x00, 0x00, 0xFF};
  TEST_ASSERT_EQUAL_HEX8(0xFC, frameChecksum(ID_MASTER_REQ, other));
}

void test_temperatures() {
  TEST_ASSERT_EQUAL_FLOAT(19.0f, decodeTemp(0x0B68));
  TEST_ASSERT_TRUE(std::isnan(decodeTemp(0)));
  TEST_ASSERT_TRUE(std::isnan(decodeTemp(0x0AAA)));
  TEST_ASSERT_EQUAL_UINT16(3130, encodeTemp(40));
  TEST_ASSERT_EQUAL_UINT16(4730, encodeTemp(200));
  TEST_ASSERT_EQUAL_UINT16(0, encodeTemp(4));
}

void test_decode_reference_status() {
  State s;
  TEST_ASSERT_TRUE(decodeStatus(STATUS_EXAMPLE, sizeof STATUS_EXAMPLE, s));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.roomTarget);
  TEST_ASSERT_EQUAL_UINT8(MODE_OFF, s.heatingMode);
  TEST_ASSERT_EQUAL_UINT8(MIX_GAS, s.energyMix);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 19.0f, s.waterCurrent);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.2f, s.roomCurrent);
  TEST_ASSERT_EQUAL_UINT8(0, s.waterTarget);

  uint8_t bad[38];
  memcpy(bad, STATUS_EXAMPLE, 38);
  bad[13] ^= 1;
  TEST_ASSERT_FALSE(decodeStatus(bad, 38, s));
}

void test_status_roundtrip() {
  State s;
  s.roomTarget = 21;
  s.heatingMode = MODE_BOOST;
  s.waterTarget = 60;
  s.energyMix = MIX_BOTH;
  s.elPower = 1800;
  s.roomCurrent = 3.5f;  // unter 5 °C muss erhalten bleiben
  s.waterCurrent = 48.3f;
  s.opStatus = 5;
  s.errorCode = 517;
  uint8_t buf[BUFFER_LEN];
  encodeStatus(s, 7, buf);
  State d;
  TEST_ASSERT_TRUE(decodeStatus(buf, sizeof buf, d));
  TEST_ASSERT_EQUAL_FLOAT(21.0f, d.roomTarget);
  TEST_ASSERT_EQUAL_UINT8(MODE_BOOST, d.heatingMode);
  TEST_ASSERT_EQUAL_UINT8(60, d.waterTarget);
  TEST_ASSERT_EQUAL_UINT8(MIX_BOTH, d.energyMix);
  TEST_ASSERT_EQUAL_UINT16(1800, d.elPower);
  TEST_ASSERT_FLOAT_WITHIN(0.06f, 3.5f, d.roomCurrent);
  TEST_ASSERT_FLOAT_WITHIN(0.06f, 48.3f, d.waterCurrent);
  TEST_ASSERT_EQUAL_UINT16(517, d.errorCode);
}

void test_command_keeps_unchanged_fields() {
  State cur;
  decodeStatus(STATUS_EXAMPLE, sizeof STATUS_EXAMPLE, cur);
  cur.payload[3] = 0x5A;  // unbekanntes Byte wird zurückgeschrieben
  Command c;
  c.hasWater = true;
  c.waterTarget = 200;
  uint8_t buf[BUFFER_LEN];
  encodeCommand(cur, c, 9, buf);
  TEST_ASSERT_EQUAL_HEX8(0x0C, buf[10]);
  TEST_ASSERT_EQUAL_HEX8(0x32, buf[11]);
  TEST_ASSERT_EQUAL_HEX8(9, buf[12]);
  TEST_ASSERT_EQUAL_HEX8(0x5A, buf[HEADER_LEN + 3]);
  Command d;
  TEST_ASSERT_TRUE(decodeCommand(buf, sizeof buf, d));
  TEST_ASSERT_EQUAL_UINT8(200, d.waterTarget);
  TEST_ASSERT_EQUAL_UINT8(MIX_GAS, d.energyMix);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d.roomTarget);
}

void test_normalize_rules() {
  State cur;  // aus, Gas
  Command c;
  c.hasRoom = true;
  c.roomTarget = 21;
  normalize(cur, 0, c);
  TEST_ASSERT_TRUE(c.hasMode);
  TEST_ASSERT_EQUAL_UINT8(MODE_ECO, c.heatingMode);  // einschalten braucht Heizstufe

  Command off;
  off.hasMode = true;
  off.heatingMode = MODE_OFF;
  normalize(cur, 0, off);
  TEST_ASSERT_TRUE(off.hasRoom);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, off.roomTarget);

  Command eco;
  eco.hasMode = true;
  eco.heatingMode = MODE_ECO;
  normalize(cur, 22, eco);
  TEST_ASSERT_EQUAL_FLOAT(22.0f, eco.roomTarget);  // letzter Sollwert

  Command el;
  el.hasEl = true;
  el.elPower = 900;
  normalize(cur, 0, el);
  TEST_ASSERT_EQUAL_UINT8(MIX_BOTH, el.energyMix);  // Gas + Elektro -> Mix

  Command mix;
  mix.hasMix = true;
  mix.energyMix = MIX_ELECTRIC;
  normalize(cur, 0, mix);
  TEST_ASSERT_EQUAL_UINT16(900, mix.elPower);

  Command gas;
  gas.hasMix = true;
  gas.energyMix = MIX_GAS;
  State elec;
  elec.energyMix = MIX_ELECTRIC;
  elec.elPower = 1800;
  normalize(elec, 0, gas);
  TEST_ASSERT_EQUAL_UINT16(0, gas.elPower);
}

void test_parse_set() {
  Command c;
  char err[48];
  TEST_ASSERT_TRUE(parseSetArgs("sp=21 mode=boost ww=60 mix=mix el=1800", c, err, sizeof err));
  TEST_ASSERT_EQUAL_FLOAT(21.0f, c.roomTarget);
  TEST_ASSERT_EQUAL_UINT8(MODE_BOOST, c.heatingMode);
  TEST_ASSERT_EQUAL_UINT8(60, c.waterTarget);
  TEST_ASSERT_EQUAL_UINT8(MIX_BOTH, c.energyMix);
  TEST_ASSERT_EQUAL_UINT16(1800, c.elPower);
  TEST_ASSERT_TRUE(parseSetArgs("sp=0", c, err, sizeof err));
  TEST_ASSERT_FALSE(parseSetArgs("sp=3", c, err, sizeof err));
  TEST_ASSERT_FALSE(parseSetArgs("ww=50", c, err, sizeof err));
  TEST_ASSERT_FALSE(parseSetArgs("el=1000", c, err, sizeof err));
  TEST_ASSERT_FALSE(parseSetArgs("mode=high", c, err, sizeof err));
  TEST_ASSERT_FALSE(parseSetArgs("gas=1", c, err, sizeof err));
}

void test_format_state() {
  State s;
  decodeStatus(STATUS_EXAMPLE, sizeof STATUS_EXAMPLE, s);
  char line[200];
  formatState(&s, line, sizeof line);
  TEST_ASSERT_EQUAL_STRING(
      "STATE link=1 room=25.2 sp=0.0 mode=off water=19.0 ww=0 mix=gas el=0 status=0 err=0",
      line);
  formatJson(&s, line, sizeof line);
  TEST_ASSERT_EQUAL_STRING(
      "{\"room\":25.2,\"sp\":0.0,\"mode\":\"off\",\"water\":19.0,\"ww\":0,\"mix\":\"gas\","
      "\"el\":0,\"status\":0,\"err\":0}",
      line);
  formatState(nullptr, line, sizeof line);
  TEST_ASSERT_EQUAL_STRING("STATE link=0", line);
}

void test_inetbox_answers() {
  InetBox box;
  uint8_t r[8];
  const uint8_t readId[8] = {0x7F, 0x06, 0xB2, 0x00, 0x17, 0x46, 0x00, 0x1F};
  box.onMasterRequest(readId);
  TEST_ASSERT_TRUE(box.nextResponse(r));
  const uint8_t expectId[8] = {0x03, 0x06, 0xF2, 0x17, 0x46, 0x00, 0x1F, 0x00};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expectId, r, 8);

  const uint8_t heartbeat[8] = {0x03, 0x03, 0xB9, 0x00, 0x1F, 0xFF, 0xFF, 0xFF};
  box.onMasterRequest(heartbeat);
  TEST_ASSERT_TRUE(box.nextResponse(r));
  TEST_ASSERT_EQUAL_HEX8(0xF9, r[2]);

  const uint8_t foreign[8] = {0x01, 0x06, 0xB2, 0x00, 0x17, 0x46, 0x00, 0x1F};
  box.onMasterRequest(foreign);  // andere NAD: keine Antwort
  TEST_ASSERT_FALSE(box.nextResponse(r));

  // Ohne Status kein Update-Flag, auch wenn ein Befehl wartet
  Command c;
  c.hasWater = true;
  c.waterTarget = 40;
  box.queue(c);
  box.updateResponse(r);
  TEST_ASSERT_EQUAL_HEX8(0xFE, r[0]);
}

void test_full_exchange_with_simulated_cp_plus() {
  InetBox box;
  CpPlusSim cp;
  cp.cycle(box, 1);
  TEST_ASSERT_TRUE(box.hasStatus());
  TEST_ASSERT_EQUAL_UINT8(MODE_OFF, box.state().heatingMode);

  Command c;
  c.hasRoom = true;
  c.roomTarget = 22;
  c.hasWater = true;
  c.waterTarget = 60;
  normalize(box.state(), 0, c);
  box.queue(c);
  uint8_t u[8];
  box.updateResponse(u);
  TEST_ASSERT_EQUAL_HEX8(0xFF, u[0]);

  cp.cycle(box, 1);  // holt Befehl ab und sendet neuen Status
  TEST_ASSERT_EQUAL_UINT32(1, cp.commandsApplied());
  TEST_ASSERT_FALSE(box.commandPending());
  TEST_ASSERT_TRUE(commandSatisfied(box.state(), c));
  TEST_ASSERT_EQUAL_UINT8(MODE_ECO, box.state().heatingMode);

  for (int i = 0; i < 500; i++) cp.cycle(box, 1);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 21.7f, box.state().roomCurrent);  // regelt 21,5..22
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 60.0f, box.state().waterCurrent);
}

void test_upload_frames_layout() {
  InetBox box;
  CpPlusSim cp;
  cp.cycle(box, 1);
  Command c;
  c.hasWater = true;
  c.waterTarget = 40;
  box.queue(c);
  const uint8_t req[8] = {0x03, 0x03, 0xBA, 0x00, 0x1F, 0xFF, 0xFF, 0xFF};
  box.onMasterRequest(req);
  uint8_t r[8];
  TEST_ASSERT_TRUE(box.nextResponse(r));
  const uint8_t first[8] = {0x03, 0x10, 0x29, 0xFA, 0x00, 0x1F, 0x00, 0x1E};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(first, r, 8);
  for (int i = 0; i < 6; i++) {
    TEST_ASSERT_TRUE(box.nextResponse(r));
    TEST_ASSERT_EQUAL_HEX8(0x21 + i, r[1]);
  }
  TEST_ASSERT_FALSE(box.nextResponse(r));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_pid);
  RUN_TEST(test_frame_checksums_from_reference);
  RUN_TEST(test_temperatures);
  RUN_TEST(test_decode_reference_status);
  RUN_TEST(test_status_roundtrip);
  RUN_TEST(test_command_keeps_unchanged_fields);
  RUN_TEST(test_normalize_rules);
  RUN_TEST(test_parse_set);
  RUN_TEST(test_format_state);
  RUN_TEST(test_inetbox_answers);
  RUN_TEST(test_full_exchange_with_simulated_cp_plus);
  RUN_TEST(test_upload_frames_layout);
  return UNITY_END();
}
