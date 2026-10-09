// Advertisements and the local GATT application as the API takes them: checked and normalised
// before anything reaches the bus.
#include "adv.h"
#include "check.h"
#include "gatt_server.h"
#include "uuids.h"

using namespace btb;
using json = nlohmann::json;

namespace {

void test_adv_spec() {
  AdvSpec s;
  std::string err;
  CHECK(adv_spec_from_json(json::object(), &s, &err));
  CHECK_EQ(s.type, std::string("peripheral"));
  CHECK(adv_spec_from_json(json::parse(R"({
      "type": "broadcast", "local_name": "btbench", "service_uuids": ["180D", "0x180f"],
      "manufacturer_data": {"0x004c": "0215aabb", "89": "01"}, "service_data": {"fe2c": "00 11"},
      "appearance": "0x0341", "tx_power": 4, "discoverable": false, "includes": ["tx-power"],
      "min_interval_ms": 100, "max_interval_ms": 150, "timeout_s": 30})"), &s, &err));
  CHECK(err.empty());
  CHECK_EQ(s.type, std::string("broadcast"));
  CHECK_EQ(s.service_uuids.size(), size_t(2));
  CHECK_EQ(s.service_uuids[0], uuid_full("180d"));
  CHECK_EQ(s.manufacturer_data.at(0x004c).size(), size_t(4));
  CHECK_EQ(s.manufacturer_data.at(89)[0], 1);
  CHECK_EQ(s.service_data.at(uuid_full("fe2c")).size(), size_t(2));
  CHECK_EQ(s.appearance, 0x341);
  CHECK(s.has_discoverable && !s.discoverable);
  // Round trip: what GET shows is what was asked for, normalised.
  const json j = adv_spec_json(s);
  CHECK_EQ(j["manufacturer_data"]["0x004c"].get<std::string>(), std::string("0215aabb"));
  CHECK_EQ(j["service_uuids"][1].get<std::string>(), std::string("180f"));
  CHECK_EQ(j["appearance"].get<std::string>(), std::string("0x0341"));
  AdvSpec again;
  CHECK(adv_spec_from_json(j, &again, &err));
  CHECK(again.manufacturer_data == s.manufacturer_data);
  CHECK_EQ(again.min_interval_ms, 100u);
}

void test_adv_rejects() {
  AdvSpec s;
  std::string err;
  CHECK(!adv_spec_from_json(json{{"type", "beacon"}}, &s, &err));
  CHECK(!adv_spec_from_json(json{{"service_uuids", {"nope"}}}, &s, &err));
  CHECK(!adv_spec_from_json(json{{"manufacturer_data", {{"0x10000", "00"}}}}, &s, &err));
  CHECK(!adv_spec_from_json(json{{"manufacturer_data", {{"76", "0g"}}}}, &s, &err));
  CHECK(!adv_spec_from_json(json{{"tx_power", 30}}, &s, &err));
  CHECK(!adv_spec_from_json(json{{"includes", {"battery"}}}, &s, &err));
  CHECK(!adv_spec_from_json(json{{"min_interval_ms", 200}, {"max_interval_ms", 100}}, &s, &err));
  CHECK(!adv_spec_from_json(json{{"min_interval_ms", 5}}, &s, &err));
  CHECK(!adv_spec_from_json(json{{"local_name", std::string(30, 'x')}}, &s, &err));
  CHECK(!adv_spec_from_json(json::array(), &s, &err));
  // Too much for one advertisement: said here, not as BlueZ's "Invalid Length".
  CHECK(!adv_spec_from_json(json{{"manufacturer_data", {{"76", std::string(500, 'a')}}}}, &s, &err));
  CHECK(err.find("bytes") != std::string::npos);
}

void test_gatt_app() {
  GattAppDef d;
  std::string err;
  CHECK(gatt_app_from_json(gatt_app_example(), &d, &err));
  CHECK(err.empty());
  CHECK_EQ(d.services.size(), size_t(3));
  CHECK_EQ(d.services[0].uuid, uuid_full("180d"));
  CHECK(d.services[0].characteristics[0].counter);
  CHECK_EQ(d.services[0].characteristics[0].period_ms, 1000u);
  CHECK_EQ(d.services[2].characteristics[0].value, std::vector<uint8_t>({'e', 'c', 'h', 'o'}));
  CHECK_EQ(d.services[2].characteristics[0].descriptors.size(), size_t(1));
  // Round trip.
  GattAppDef again;
  CHECK(gatt_app_from_json(gatt_app_json(d), &again, &err));
  CHECK_EQ(again.services.size(), d.services.size());
  CHECK_EQ(again.services[2].characteristics[0].value, d.services[2].characteristics[0].value);

  CHECK(!gatt_app_from_json(json::object(), &d, &err));
  CHECK(!gatt_app_from_json(json::parse(R"({"services":[{"uuid":"180d","characteristics":[{"uuid":"2a37"}]}]})"), &d, &err));
  CHECK(err.find("no flags") != std::string::npos);
  CHECK(!gatt_app_from_json(json::parse(R"({"services":[{"uuid":"180d","characteristics":[{"uuid":"2a37","flags":["fly"]}]}]})"), &d, &err));
  CHECK(!gatt_app_from_json(json::parse(R"({"services":[{"uuid":"x","characteristics":[]}]})"), &d, &err));
  CHECK(!gatt_app_from_json(json::parse(R"({"services":[{"uuid":"180d","characteristics":[{"uuid":"2a37","flags":["notify"],
       "descriptors":[{"uuid":"2902"}]}]}]})"), &d, &err));
  CHECK(!gatt_app_from_json(json::parse(R"({"services":[{"uuid":"180d","characteristics":[{"uuid":"2a37","flags":["notify"],
       "value":"0102030405","counter":true}]}]})"), &d, &err));
}

void test_counter() {
  CHECK_EQ(gatt_counter_bytes(0x1234, 2), std::vector<uint8_t>({0x34, 0x12}));
  CHECK_EQ(gatt_counter_bytes(0x1ff, 1), std::vector<uint8_t>({0xff}));
  CHECK_EQ(gatt_counter_bytes(1, 0).size(), size_t(1));
  CHECK_EQ(gatt_counter_bytes(1, 9).size(), size_t(4));
}

}  // namespace

int main() {
  test_adv_spec();
  test_adv_rejects();
  test_gatt_app();
  test_counter();
  return report("test_le");
}
