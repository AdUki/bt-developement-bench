// The Bluetooth manager's decisions: what the agent answers by itself, which IO capabilities and
// discovery filters it takes, the request's shape, and which adapter settings it keeps and how it
// puts them back. The bus traffic is tools/fake-bluez's.
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

  // Every key BlueZ takes: pathloss instead of RSSI (never both), discoverable, a pattern, and
  // auto-connect, which needs the pattern.
  CHECK(bt_scan_filter_from_json(json{{"transport", "bredr"}, {"pathloss", 80}, {"discoverable", true},
                                      {"pattern", "C0:FF"}, {"auto_connect", true}},
                                 &f, &err));
  CHECK(f.has_pathloss && f.pathloss == 80 && !f.has_rssi);
  CHECK(f.discoverable && f.auto_connect && f.has_pattern);
  CHECK_EQ(f.pattern, std::string("C0:FF"));
  const json all = bt_scan_filter_json(f);
  CHECK_EQ(all["pathloss"].get<int>(), 80);
  CHECK(all["rssi"].is_null());
  CHECK_EQ(all["pattern"].get<std::string>(), std::string("C0:FF"));
  CHECK(all["discoverable"].get<bool>() && all["auto_connect"].get<bool>());
  // "" is a pattern too (it matches everything): it is kept, not dropped as unset.
  CHECK(bt_scan_filter_from_json(json{{"pattern", ""}}, &f, &err));
  CHECK(f.has_pattern && f.pattern.empty());
  CHECK(bt_scan_filter_from_json(json::object(), &f, &err));
  CHECK(!f.has_pattern && bt_scan_filter_json(f)["pattern"].is_null());
  CHECK(!bt_scan_filter_from_json(json{{"rssi", -70}, {"pathloss", 80}}, &f, &err));
  CHECK(err.find("either-or") != std::string::npos);
  CHECK(!bt_scan_filter_from_json(json{{"pathloss", 138}}, &f, &err));
  CHECK(!bt_scan_filter_from_json(json{{"pathloss", -1}}, &f, &err));
  CHECK(!bt_scan_filter_from_json(json{{"auto_connect", true}}, &f, &err));
  CHECK(!bt_scan_filter_from_json(json{{"pattern", std::string(249, 'x')}}, &f, &err));
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

std::string names(const DDict& d) {
  std::string out;
  for (const auto& p : d) out += (out.empty() ? "" : ",") + p.first;
  return out;
}

void test_kept_adapter_settings() {
  // What was set is kept, on top of what was kept before; nothing else is invented.
  json k = bt_keep_adapter_settings(json::object(), json{{"powered", false}}, 0);
  CHECK_EQ(k.dump(), std::string(R"({"powered":false})"));
  k = bt_keep_adapter_settings(k, json{{"pairable", false}, {"alias", "bench"}}, 0);
  CHECK_EQ(k.dump(), std::string(R"({"alias":"bench","pairable":false,"powered":false})"));
  // A lasting Discoverable is a setting; one with a timeout in force is a moment, kept as off.
  CHECK(bt_keep_adapter_settings(json::object(), json{{"discoverable", true}}, 0)["discoverable"].get<bool>());
  CHECK(!bt_keep_adapter_settings(json::object(), json{{"discoverable", true}}, 180)["discoverable"].get<bool>());
  CHECK(!bt_keep_adapter_settings(json::object(), json{{"discoverable", true}, {"discoverable_timeout_s", 60}}, 60)
             ["discoverable"].get<bool>());
  // A broken kept entry starts over.
  CHECK_EQ(bt_keep_adapter_settings(json("x"), json{{"pairable", true}}, 0).dump(), std::string(R"({"pairable":true})"));

  // Restored in BlueZ's order: Powered first, the timeout before Discoverable.
  const json all{{"discoverable", true}, {"pairable", false}, {"discoverable_timeout_s", 0}, {"alias", "b"}, {"powered", true}};
  CHECK_EQ(names(bt_restore_adapter_props(all)), std::string("Powered,Alias,DiscoverableTimeout,Pairable,Discoverable"));
  // An adapter kept off is only switched off: BlueZ refuses the rest while it is.
  json off = all;
  off["powered"] = false;
  const DDict p = bt_restore_adapter_props(off);
  CHECK_EQ(names(p), std::string("Powered"));
  CHECK_EQ(p[0].second.i, 0);
  // Nothing kept, nothing written; values of the wrong type are skipped.
  CHECK(bt_restore_adapter_props(json::object()).empty());
  CHECK(bt_restore_adapter_props(json{{"powered", "yes"}, {"discoverable_timeout_s", -1}}).empty());
}

void test_bredr_connected() {
  // read_json's shape: interface → property → value.
  const json objs = {
      {"/org/bluez/hci0", {{"org.bluez.Adapter1", {{"Powered", true}}}}},
      // A speaker on BR/EDR (with the bearer object): reconnected.
      {"/org/bluez/hci0/dev_50_1E_2D_95_16_34",
       {{"org.bluez.Device1", {{"Address", "50:1e:2d:95:16:34"}, {"Paired", true}, {"Connected", true}, {"Class", 0x240414}}},
        {"org.bluez.Bearer.BREDR1", {{"Connected", true}}},
        {"org.bluez.Bearer.LE1", {{"Connected", true}}}}},
      // Dual mode, connected over LE only: BlueZ's auto-connect has it.
      {"/org/bluez/hci0/dev_66_E5_17_5C_19_5D",
       {{"org.bluez.Device1", {{"Address", "66:E5:17:5C:19:5D"}, {"Paired", true}, {"Connected", true}, {"Class", 1}}},
        {"org.bluez.Bearer.BREDR1", {{"Connected", false}}}}},
      // No bearer objects (an older BlueZ): connected with a class of device counts, LE-only does not.
      {"/org/bluez/hci0/dev_AA_00_00_00_00_01",
       {{"org.bluez.Device1", {{"Address", "AA:00:00:00:00:01"}, {"Paired", true}, {"Connected", true}, {"Class", 0x5a020c}}}}},
      {"/org/bluez/hci0/dev_AA_00_00_00_00_02",
       {{"org.bluez.Device1", {{"Address", "AA:00:00:00:00:02"}, {"Paired", true}, {"Connected", true}}}}},
      // Not paired, or on another adapter: never.
      {"/org/bluez/hci0/dev_AA_00_00_00_00_03",
       {{"org.bluez.Device1", {{"Address", "AA:00:00:00:00:03"}, {"Paired", false}, {"Connected", true}, {"Class", 1}}}}},
      {"/org/bluez/hci1/dev_AA_00_00_00_00_04",
       {{"org.bluez.Device1", {{"Address", "AA:00:00:00:00:04"}, {"Paired", true}, {"Connected", true}, {"Class", 1}}}}},
  };
  std::vector<std::string> got = bt_bredr_connected(objs, "/org/bluez/hci0");
  std::string all;
  for (const std::string& a : got) all += a + " ";
  CHECK_EQ(all, std::string("50:1E:2D:95:16:34 AA:00:00:00:00:01 "));
  CHECK(bt_bredr_connected(objs, "").empty());
  CHECK(bt_bredr_connected(json::object(), "/org/bluez/hci0").empty());
}

int main() {
  uuid_names().load_text(R"({"uuid16": {"110d": "Advanced Audio Distribution"}})", nullptr);
  test_agent_policy();
  test_capabilities();
  test_scan_filter();
  test_request_json();
  test_kept_adapter_settings();
  test_bredr_connected();
  return report("test_bluetooth");
}
