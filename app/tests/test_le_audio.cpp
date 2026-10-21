// LE Audio broadcasts: what a broadcast request becomes on D-Bus, and the API's view of BlueZ's
// broadcast objects (the board's own BIG, a broadcaster in range) and PipeWire's nodes for them.
#include "audio/endpoints.h"
#include "check.h"
#include "le_audio.h"

using namespace btb;
using json = nlohmann::json;

namespace {

// A byte array as read_json gives one: unsigned numbers.
json ay(std::vector<unsigned> v) { return json(v); }

void test_config() {
  LeBcastConfig c;
  std::string err;
  CHECK(le_bcast_config_from_json(json{{"preset", "48_2_2"}, {"channels", 2}}, 0, &c, &err));
  // 48 kHz, 10 ms, front left + right, 100 octets a channel: one SDU of 200 bytes every 10 ms.
  CHECK_EQ(c.capabilities, (std::vector<uint8_t>{2, 1, 8, 2, 2, 1, 5, 3, 3, 0, 0, 0, 3, 4, 100, 0}));
  CHECK_EQ(c.sdu, uint16_t(200));
  CHECK_EQ(c.interval_us, 10000u);
  CHECK_EQ(c.preset.rtn, uint8_t(4));
  CHECK_EQ(c.preset.latency_ms, uint16_t(65));
  CHECK(!c.encrypted);
  CHECK(le_bcast_config_from_json(json{{"preset", "16_1_1"}, {"channels", 1}, {"code", "bench"}}, 1, &c, &err));
  CHECK_EQ(c.capabilities[2], uint8_t(3));  // 16 kHz
  CHECK_EQ(c.capabilities[5], uint8_t(0));  // 7.5 ms
  CHECK_EQ(c.sdu, uint16_t(30));
  CHECK(c.encrypted);
  CHECK_EQ(c.code[0], uint8_t('b'));
  CHECK_EQ(c.code[15], uint8_t(0));
  CHECK(!le_bcast_config_from_json(json{{"preset", "44_1_1"}}, 0, &c, &err));
  CHECK(!le_bcast_config_from_json(json{{"channels", 3}}, 0, &c, &err));
  std::vector<uint8_t> code;
  CHECK(le_bcast_code("0x000102030405060708090a0b0c0d0e0f", &code, &err));
  CHECK_EQ(code[15], uint8_t(15));
  CHECK(!le_bcast_code("seventeen bytes!!", &code, &err));
}

void test_model() {
  const json objs = {
      {"/org/bluez/hci0", {{"org.bluez.Adapter1", {{"Address", "44:A3:BB:36:5E:2E"}, {"Alias", "bench"}}}}},
      {"/org/bluez/hci0/pac_bcast0", {{"org.bluez.MediaEndpoint1", {{"Device", "/org/bluez/hci0"}, {"Codec", 6}}}}},
      {"/org/bluez/hci0/pac_bcast0/fd1",
       {{"org.bluez.MediaTransport1",
         {{"UUID", "00001852-0000-1000-8000-00805f9b34fb"}, {"State", "active"}, {"Codec", 6},
          {"Configuration", ay({2, 1, 8, 2, 2, 1, 5, 3, 1, 0, 0, 0, 3, 4, 100, 0})},
          {"QoS", {{"BIG", 0}, {"BIS", 1}, {"SDU", 100}, {"PHY", 2}, {"Encryption", 0}}}}}}},
      {"/org/bluez/hci0/dev_84_17_15_2D_00_20",
       {{"org.bluez.Device1",
         {{"Address", "84:17:15:2D:00:20"}, {"Alias", "RTL8763E"}, {"RSSI", -60},
          {"ServiceData", {{"00001852-0000-1000-8000-00805f9b34fb", ay({0xac, 0x27, 0xa3})}}}}}}},
      {"/org/bluez/hci0/dev_84_17_15_2D_00_20/sid0/bis1/fd0",
       {{"org.bluez.MediaTransport1",
         {{"UUID", "00001851-0000-1000-8000-00805f9b34fb"}, {"State", "idle"}, {"Codec", 6}, {"Location", 3},
          // As the BASE has it, with its length in front.
          {"Configuration", ay({19, 2, 1, 8, 2, 2, 1, 3, 4, 100, 0, 2, 5, 1, 5, 3, 3, 0, 0, 0})},
          {"QoS", {{"BIG", 0}, {"BIS", 1}, {"Encryption", 1}}}}}}},
      // A remote Broadcast Assistant's source: not the console's to drive.
      {"/org/bluez/hci0/dev_84_17_15_2D_00_20/src_84_17_15_2D_00_20/sid0/bis1",
       {{"org.bluez.MediaTransport1", {{"State", "idle"}}}}},
      {"/org/bluez/hci0/dev_0C_32_3A_D6_C3_44", {{"org.bluez.Device1", {{"Address", "0C:32:3A:D6:C3:44"}}}}}};
  const json controllers = {{"controllers", {{{"address", "44:A3:BB:36:5E:2E"}, {"supported", {"le", "iso-broadcaster", "sync-receiver"}}}}}};
  const json endpoints = {{"sinks", {{{"id", "bluez_output.00_00_00_00_00_00.5.1"}, {"transport", "/org/bluez/hci0/pac_bcast0/fd1"}}}},
                          {"sources", json::array()}};
  const json m = model_le_audio(objs, controllers, endpoints);
  CHECK_EQ(m["adapters"].size(), size_t(1));
  CHECK(m["adapters"][0]["broadcaster"].get<bool>());
  CHECK_EQ(m["adapters"][0]["pac"].get<std::string>(), std::string("/org/bluez/hci0/pac_bcast0"));
  CHECK_EQ(m["broadcasts"].size(), size_t(1));
  CHECK_EQ(m["broadcasts"][0]["big"].get<int>(), 0);
  CHECK_EQ(m["broadcasts"][0]["node"].get<std::string>(), std::string("bluez_output.00_00_00_00_00_00.5.1"));
  CHECK_EQ(m["broadcasts"][0]["qos"]["phy"].get<std::string>(), std::string("2M"));
  CHECK_EQ(m["sources"].size(), size_t(1));  // the phone does not broadcast
  const json& s = m["sources"][0];
  CHECK_EQ(s["broadcast_id"].get<std::string>(), std::string("A327AC"));
  CHECK(s["encrypted"].get<bool>());
  CHECK_EQ(s["streams"].size(), size_t(1));
  CHECK_EQ(s["streams"][0]["codec"]["octets_per_frame"].get<int>(), 100);
  CHECK_EQ(s["streams"][0]["codec"]["channels"].get<int>(), 2);
  CHECK(s["streams"][0]["node"].is_null());
  // Received, its first BIS plays through the device's own card: the device's capture node.
  json objs2 = objs;
  objs2["/org/bluez/hci0/dev_84_17_15_2D_00_20/sid0/bis1/fd0"]["org.bluez.MediaTransport1"]["State"] = "active";
  const json ep2 = {{"sinks", json::array()},
                    {"sources", {{{"id", "bluez_input.84_17_15_2D_00_20.0"}, {"kind", "bluetooth"},
                                  {"address", "84:17:15:2D:00:20"}, {"transport", nullptr}}}}};
  CHECK_EQ(model_le_audio(objs2, controllers, ep2)["sources"][0]["streams"][0]["node"].get<std::string>(),
           std::string("bluez_input.84_17_15_2D_00_20.0"));
  CHECK_EQ(le_free_big(m["broadcasts"]), uint8_t(1));
}

void test_endpoints() {
  // PipeWire's device per BIS has the transport as its path; its node is named after 00:00:...
  const json dump = {
      {{"id", 33}, {"type", "PipeWire:Interface:Device"},
       {"info", {{"props", {{"device.api", "bluez5"}, {"api.bluez5.path", "/org/bluez/hci0/pac_bcast0/fd2"},
                            {"device.name", "bluez_card.00_00_00_00_00_00.3986"}}}}}},
      {{"id", 66}, {"type", "PipeWire:Interface:Node"},
       {"info", {{"props", {{"media.class", "Audio/Sink"}, {"node.name", "bluez_output.00_00_00_00_00_00.3986.1"},
                            {"node.description", "00 00 00 00 00 00.3986"}, {"device.api", "bluez5"},
                            {"device.id", 33}, {"api.bluez5.address", "00:00:00:00:00:00.3986"}}}}}}};
  const json e = audio::endpoints_from_pw_dump(dump);
  CHECK_EQ(e["sinks"].size(), size_t(1));
  CHECK_EQ(e["sinks"][0]["label"].get<std::string>(), std::string("Auracast broadcast"));
  CHECK_EQ(e["sinks"][0]["transport"].get<std::string>(), std::string("/org/bluez/hci0/pac_bcast0/fd2"));
  CHECK(e["sinks"][0]["address"].is_null());
  CHECK_EQ(audio::cards_from_pw_dump(dump)["devices"].size(), size_t(0));
}

void test_metadata() {
  // What a Realtek broadcaster sends: media context and a CCID list, nothing about what plays.
  json m = le_metadata_json({3, 2, 4, 0, 2, 5, 0x20});
  CHECK_EQ(m["contexts"].get<int>(), 4);
  CHECK(!m.contains("program_info"));
  // One that says what plays: Program_Info after the context.
  m = le_metadata_json({3, 2, 4, 0, 13, 3, 'G', 'r', 'o', 'o', 'v', 'e', ' ', 'S', 'a', 'l', 'a', 'd'});
  CHECK_EQ(m["program_info"].get<std::string>(), std::string("Groove Salad"));
  CHECK_EQ(m["contexts"].get<int>(), 4);
  // A truncated LTV ends the walk rather than reading past the end.
  CHECK(le_metadata_json({9, 3, 'a'}).empty());
}

}  // namespace

int main() {
  test_config();
  test_metadata();
  test_model();
  test_endpoints();
  return report("le_audio");
}
