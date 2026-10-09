// The Bluetooth manager's decisions: what the agent answers by itself, which IO capabilities and
// discovery filters it takes, and the request's shape. The bus traffic is tools/fake-bluez's.
#include "bluetooth.h"
#include "check.h"
#include "uuids.h"

using namespace btb;
using json = nlohmann::json;

namespace {

void test_agent_policy() {
  // auto: everything but a passkey to type, which cannot be guessed.
  CHECK(bt_agent_policy("auto", BtAsk::Confirm) == BtVerdict::Accept);
  CHECK(bt_agent_policy("auto", BtAsk::Authorize) == BtVerdict::Accept);
  CHECK(bt_agent_policy("auto", BtAsk::Service) == BtVerdict::Accept);
  CHECK(bt_agent_policy("auto", BtAsk::Pin) == BtVerdict::Accept);
  CHECK(bt_agent_policy("auto", BtAsk::Passkey) == BtVerdict::Ask);
  // ask: every question reaches the operator.
  for (BtAsk a : {BtAsk::Confirm, BtAsk::Authorize, BtAsk::Service, BtAsk::Pin, BtAsk::Passkey})
    CHECK(bt_agent_policy("ask", a) == BtVerdict::Ask);
}

void test_capabilities() {
  CHECK(bt_agent_capability_ok("KeyboardDisplay"));
  CHECK(bt_agent_capability_ok("NoInputNoOutput"));
  CHECK(!bt_agent_capability_ok("keyboarddisplay"));
  CHECK(!bt_agent_capability_ok(""));
}

void test_scan_filter() {
  BtScanFilter f;
  std::string err;
  CHECK(bt_scan_filter_from_json(json::object(), &f, &err));
  CHECK_EQ(f.transport, std::string("auto"));
  CHECK(!f.has_rssi);
  CHECK(bt_scan_filter_from_json(json{{"transport", "le"}, {"rssi", -70}, {"uuids", {"180d"}},
                                      {"duplicate_data", false}},
                                 &f, &err));
  CHECK_EQ(f.transport, std::string("le"));
  CHECK(f.has_rssi && f.rssi == -70);
  CHECK(!f.duplicate_data);
  CHECK_EQ(f.uuids[0], uuid_full("180d"));
  const json back = bt_scan_filter_json(f);
  CHECK_EQ(back["uuids"][0].get<std::string>(), std::string("180d"));
  CHECK_EQ(back["rssi"].get<int>(), -70);

  BtScanFilter keep = f;
  CHECK(!bt_scan_filter_from_json(json{{"transport", "usb"}}, &f, &err));
  CHECK(!bt_scan_filter_from_json(json{{"rssi", -200}}, &f, &err));
  CHECK(!bt_scan_filter_from_json(json{{"uuids", {"nope"}}}, &f, &err));
  CHECK(!bt_scan_filter_from_json(json{{"rssi", "loud"}}, &f, &err));
  CHECK_EQ(f.transport, keep.transport);  // a rejected filter changes nothing
}

void test_request_json() {
  BtRequest r;
  r.id = 7;
  r.kind = "service";
  r.address = "AA:BB:CC:DD:EE:FF";
  r.name = "Pixel";
  r.uuid = "110d";
  r.expires_s = 42;
  const json j = bt_request_json(r);
  CHECK_EQ(j["id"].get<int>(), 7);
  CHECK_EQ(j["kind"].get<std::string>(), std::string("service"));
  CHECK_EQ(j["uuid_name"].get<std::string>(), std::string("Advanced Audio Distribution"));
  CHECK_EQ(j["expires_s"].get<int>(), 42);
}

}  // namespace

int main() {
  uuid_names().load_text(R"({"uuid16": {"110d": "Advanced Audio Distribution"}})", nullptr);
  test_agent_policy();
  test_capabilities();
  test_scan_filter();
  test_request_json();
  return report("test_bluetooth");
}
